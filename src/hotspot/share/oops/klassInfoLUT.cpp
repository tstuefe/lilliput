/*
 * Copyright (c) 2024, Red Hat, Inc. All rights reserved.
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/allocation.hpp"
//#include "oops/compressedKlass.inline.hpp"
#include "oops/instanceKlass.inline.hpp"
#include "oops/klass.hpp"
#include "oops/klassInfoLUT.inline.hpp"
#include "oops/klassInfoLUTEntry.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"
#include "utilities/ostream.hpp"

uint32_t* KlassInfoLUT::_entries = nullptr;

void KlassInfoLUT::initialize() {
  assert(UseKLUT, "?");
  assert(CompressedKlassPointers::narrow_klass_pointer_bits() <= 22, "sanity");
  // Note: this can be done a lot smarter, e.g. with spotwise mmap. We also should use large pages if possible.
  // For now, this suffices.
  _entries = NEW_C_HEAP_ARRAY(uint32_t, num_entries(), mtClass);
  for (unsigned i = 0; i < num_entries(); i++) {
    _entries[i] = KlassLUTEntry::invalid_entry;
  }
}

void KlassInfoLUT::register_klass(const Klass* k) {
  assert(UseKLUT, "?");
  const narrowKlass nk = CompressedKlassPointers::encode(const_cast<Klass*>(k)); // grr why is this nonconst
  assert(nk < num_entries(), "oob");
  KlassLUTEntry e(k);
  _entries[nk] = e.value();
#ifdef ASSERT
  // sanity checks
  {
    // We use at(), not get_entry(), since we don't want to log or count stats
    KlassLUTEntry e2(at(nk));
    assert(e2.value() == e.value(), "Sanity");
    e2.verify_against(k);
  }
  // stats
  switch (k->kind()) {
  case Klass::InstanceKlassKind:             inc_registered_IK(); break;
  case Klass::InstanceRefKlassKind:          inc_registered_IRK(); break;
  case Klass::InstanceMirrorKlassKind:       inc_registered_IMK(); break;
  case Klass::InstanceClassLoaderKlassKind:  inc_registered_ICLK(); break;
  case Klass::InstanceStackChunkKlassKind:   inc_registered_ISCK(); break;
  case Klass::TypeArrayKlassKind:            inc_registered_TAK(); break;
  case Klass::ObjArrayKlassKind:             inc_registered_OAK(); break;
  default: ShouldNotReachHere();
  };
#endif
}

#ifdef ASSERT

#define XX(xx)                      \
volatile uint64_t counter_##xx = 0; \
void KlassInfoLUT::inc_##xx() {     \
  Atomic::inc(&counter_##xx);       \
}
STATS_DO(XX)
#undef XX

void KlassInfoLUT::print_statistics(outputStream* st) {
  assert(UseKLUT, "?");
  st->print_cr("KLUT stats:");
#define XX(xx)                                \
  st->print("   " #xx ":");                   \
  st->fill_to(22);                            \
  st->print_cr(UINT64_FORMAT, counter_##xx);
STATS_DO(XX)
#undef XX
  const uint64_t hits =
#define XX(name, shortname) counter_hits_##shortname +
ALL_KLASS_KINDS_DO(XX)
#undef XX
   0;
#define PERCENTAGE_OF(x, x100) ( ((double)x * 100.0f) / x100 )
const uint64_t hits_ak = counter_hits_OAK + counter_hits_TAK;
  const uint64_t hits_ik = hits - hits_ak;
  const uint64_t no_info_hits = counter_noinfo_ICLK + counter_noinfo_IMK + counter_noinfo_IK_other;

  st->print("   IK hits total: ");
  st->fill_to(22);
  st->print_cr(UINT64_FORMAT " (%.1f%%)", hits_ik, PERCENTAGE_OF(hits_ik, hits));

  st->print("   AK hits total: ");
  st->fill_to(22);
  st->print_cr(UINT64_FORMAT " (%.1f%%)", hits_ak, PERCENTAGE_OF(hits_ak, hits));

  st->print_cr("   IK details missing in %.2f%% of all IK hits (IMK: %.2f%%, ICLK: %.2f%%, other: %.2f%%)",
               PERCENTAGE_OF(no_info_hits, hits_ik),
               PERCENTAGE_OF(counter_noinfo_IMK, hits_ik),
               PERCENTAGE_OF(counter_noinfo_ICLK, hits_ik),
               PERCENTAGE_OF(counter_noinfo_IK_other, hits_ik)
  );

  st->print("   Hits of bootloaded Klass: ");
  st->fill_to(22);
  st->print_cr(UINT64_FORMAT " (%.1f%%)", counter_hits_bootloaded, PERCENTAGE_OF(counter_hits_bootloaded, hits));
}

#ifdef KLUT_ENABLE_EXPENSIVE_STATS
void KlassInfoLUT::update_hit_stats(KlassLUTEntry klute) {
  switch (klute.kind()) {
#define XX(name, shortname) case Klass::name ## Kind: inc_hits_ ## shortname(); break;
  ALL_KLASS_KINDS_DO(XX)
#undef XX
  default: ShouldNotReachHere();
  };
  if (klute.is_instance() && !klute.ik_carries_infos()) {
    switch (klute.kind()) {
      case Klass::InstanceClassLoaderKlassKind: inc_noinfo_ICLK(); break;
      case Klass::InstanceMirrorKlassKind: inc_noinfo_IMK(); break;
      default: inc_noinfo_IK_other(); break;
    }
  }
  if (klute.bootloaded()) {
    inc_hits_bootloaded();
  }
}
#endif // KLUT_ENABLE_EXPENSIVE_STATS

#ifdef KLUT_ENABLE_EXPENSIVE_LOG
void KlassInfoLUT::log_hit(KlassLUTEntry klute) {
  //log_debug(klut)("retrieval: klute: name: %s kind: %d", k->name()->as_C_string(), k->kind());
}
#endif

#endif // ASSERT
