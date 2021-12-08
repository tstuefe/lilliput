/*
 * Copyright (c) 2020, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2020 SAP SE. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"

#include "memory/metaspace/commitLimiter.hpp"
#include "memory/metaspace/counters.hpp"
#include "memory/metaspace/metaspaceContext.hpp"
#include "memory/metaspace/metaspaceArena.hpp"
#include "memory/metaspace/metaspaceArenaGrowthPolicy.hpp"
#include "memory/metaspace/virtualSpaceList.hpp"
#include "memory/metaspace.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/objectMonitor.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/align.hpp"
#include "utilities/ostream.hpp"
#include "utilities/globalDefinitions.hpp"

#include "unittest.hpp"
#include "testutils.hpp"

using metaspace::ArenaGrowthPolicy;
using metaspace::CommitLimiter;

using metaspace::MetaspaceContext;
using metaspace::MetaspaceArena;
using metaspace::SizeAtomicCounter;
using metaspace::VirtualSpaceList;


template <class T>
class StructureHeap {

  // The underlying memory range. Will be committed on demand.
  ReservedSpace _rs;

  // The metaspace context managing the range
  MetaspaceContext* _context;

  // We create one arena for this context
  MetaspaceArena* _arena;

  // Auxiliary Stuff we need but maybe we can simplify this
  CommitLimiter _dummy_limiter;
  SizeAtomicCounter _cnt;
  Mutex* _lock;

  // Metaspace imposes an internal alignment and minimal size
  static size_t aligned_element_size() {
    return align_up(sizeof(T), sizeof(MetaWord));
  }

  // Metaspace imposes an alignment of 4M for ranges (root chunk size)
  static const size_t metaspace_range_alignment = 4 * M; // Metaspace::reserve_alignment(); non const :(

  static size_t memory_range_size(unsigned num_elements) {
    return align_up(num_elements * aligned_element_size(), metaspace_range_alignment);
  }

public:

  StructureHeap(unsigned max_elements) :
    _rs(memory_range_size(max_elements), metaspace_range_alignment, os::vm_page_size()),
    _context(NULL),
    _arena(NULL),
    _dummy_limiter(), // dont limit (we are still limited by rs size oc)
    _cnt(), _lock(NULL)
  {
    _context = MetaspaceContext::create_nonexpandable_context("my context", _rs, &_dummy_limiter);
    _lock = new Mutex(Monitor::nosafepoint, "my lock");
    const ArenaGrowthPolicy* pol = ArenaGrowthPolicy::policy_for_space_type(Metaspace::BootMetaspaceType, false);
    _arena = new MetaspaceArena(_context->cm(), pol, _lock, &_cnt, "my arena");
  }

  ~StructureHeap() {

    delete _arena;

    {
      MutexLocker fcl(Metaspace_lock, Mutex::_no_safepoint_check_flag);
      delete _context;
    }

    delete _lock;
  }

  // Return T-shaped uninitialized space
  T* allocate_space_for() {
    return (T*) _arena->allocate(aligned_element_size() / BytesPerWord);
  }

  // Release space for T
  void deallocate_space_for(T* p) {
    _arena->deallocate((MetaWord*)p, aligned_element_size() / BytesPerWord);
  }

  size_t reserve_bytes() const {
    return _rs.size();
  }

  size_t committed_bytes() const {
    return _context->vslist()->committed_words() * BytesPerWord;
  }

  bool contains(const T* p) const {
    return (const char*)p >= _rs.base() && (const char*)p < _rs.end();
  }

  void print_on(outputStream* os) const {
    os->print_cr("reserved: " SIZE_FORMAT, reserve_bytes());
    os->print_cr("committed: " SIZE_FORMAT, committed_bytes());
    _arena->print_on(os);
  }
};

TEST_VM(metaspace, structureheap) {

  const unsigned max_monitors = 5000;
  StructureHeap<ObjectMonitor> _heap(max_monitors);

  ASSERT_EQ((size_t)0, _heap.committed_bytes()); // We should have nothing allocated yet.

  ObjectMonitor** test_holder = NEW_C_HEAP_ARRAY(ObjectMonitor*, max_monitors, mtTest);

  // Fill completely
  for (unsigned i = 0; i < max_monitors; i ++) {
    ObjectMonitor* p_uninitialized = _heap.allocate_space_for();
    ASSERT_NOT_NULL(p_uninitialized);
    ASSERT_TRUE(_heap.contains(p_uninitialized));
    test_holder[i] = p_uninitialized; // We need a placement new in ObjectMonitor, or a default ctor
    // We expect committed memory to gradually increase.
    // Too lazy to test this here, just print out.
    if (i % 0x100 == 0) {
      tty->print_cr("allocated: %u, reserved: " SIZE_FORMAT ", committed: " SIZE_FORMAT,
                    i, _heap.reserve_bytes(), _heap.committed_bytes());
    }
  }

  _heap.print_on(tty);

  // We should not be able to allocate one more, since the range should be maxed out
  // Actually this accidentally works since the reserved space needs to be a multiple of 4M
 // ASSERT_NULL(_heap.allocate_space_for());

  // Release every third. This will add the space to the internal freelist.
  for (unsigned i = 0; i < max_monitors; i += 3) {
    _heap.deallocate_space_for(test_holder[i]);
    test_holder[i] = NULL;
  }

  // Allocate again. We should be able to allocate exactly what we freed, but not more.
  for (unsigned i = 0; i < max_monitors; i += 3) {
    ObjectMonitor* p_uninitialized = _heap.allocate_space_for();
    ASSERT_NOT_NULL(p_uninitialized);
    ASSERT_TRUE(_heap.contains(p_uninitialized));
    test_holder[i] = p_uninitialized;
  }

  _heap.print_on(tty);

  FREE_C_HEAP_ARRAY(ObjectMonitor*, test_holder);

}

