/*
 * Copyright (c) 1997, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_MARKWORD_HPP
#define SHARE_OOPS_MARKWORD_HPP

#include "metaprogramming/primitiveConversions.hpp"
#include "oops/compressedKlass.hpp"
#include "oops/oopsHierarchy.hpp"
#include "runtime/globals.hpp"

#include <type_traits>

// The markWord describes the header of an object.
//
// Bit-format of an object header (most significant first, big endian layout below):
//
//  32 bits:
//  --------
//             hash:25 ------------>| age:4  self-fwd:1  lock:2 (normal object)
//
//  64 bits:
//  --------
//  unused:22 hash:31 -->| unused_gap:4  age:4  self-fwd:1  lock:2 (normal object)
//
//  64 bits (with compact headers):
//  -------------------------------
//  klass:22  unused_gap:29 hash:2 -->| unused_gap:4  age:4  self-fwd:1  lock:2 (normal object)
//
//  - hash contains the identity hash value: largest value is
//    31 bits, see os::random().  Also, 64-bit vm's require
//    a hash value no bigger than 32 bits because they will not
//    properly generate a mask larger than that: see library_call.cpp
//
//  - With +UseCompactObjectHeaders:
//    hashctrl bits indicate if object has been hashed:
//    00 - never hashed
//    01 - hashed, but not expanded by GC: will recompute hash
//    10 - not hashed, but expanded; special state used only by CDS to deal with scratch classes
//    11 - hashed and expanded by GC, and hashcode has been installed in hidden field
//
//    When identityHashCode() is called, the transitions work as follows:
//    00 - set the hashctrl bits to 01, and compute the identity hash
//    01 - recompute idendity hash. When GC encounters 01 when moving an object, it will allocate an extra word, if
//         necessary, for the object copy, and install 11.
//    11 - read hashcode from field
//
//  - the two lock bits are used to describe three states: locked/unlocked and monitor.
//
//    [ptr             | 00]  locked             ptr points to real header on stack (stack-locking in use)
//    [header          | 00]  locked             locked regular object header (fast-locking in use)
//    [header          | 01]  unlocked           regular object header
//    [ptr             | 10]  monitor            inflated lock (header is swapped out, UseObjectMonitorTable == false)
//    [header          | 10]  monitor            inflated lock (UseObjectMonitorTable == true)
//    [ptr             | 11]  marked             used to mark an object
//    [0 ............ 0| 00]  inflating          inflation in progress (stack-locking in use)
//
//    We assume that stack/thread pointers have the lowest two bits cleared.
//
//  - INFLATING() is a distinguished markword value of all zeros that is
//    used when inflating an existing stack-lock into an ObjectMonitor.
//    See below for is_being_inflated() and INFLATING().

class BasicLock;
class ObjectMonitor;
class JavaThread;
class outputStream;

class markWord {
 private:
  uintptr_t _value;

 public:
  explicit markWord(uintptr_t value) : _value(value) {}

  markWord() = default;         // Doesn't initialize _value.

  // It is critical for performance that this class be trivially
  // destructable, copyable, and assignable.
  ~markWord() = default;
  markWord(const markWord&) = default;
  markWord& operator=(const markWord&) = default;

  static markWord from_pointer(void* ptr) {
    return markWord((uintptr_t)ptr);
  }
  void* to_pointer() const {
    return (void*)_value;
  }

  bool operator==(const markWord& other) const {
    return _value == other._value;
  }
  bool operator!=(const markWord& other) const {
    return !operator==(other);
  }

  // Conversion
  uintptr_t value() const { return _value; }
  uint32_t value32() const { return (uint32_t)_value; }

  // Constants
  static const int age_bits                       = 4;
  static const int lock_bits                      = 2;
  static const int self_fwd_bits                  = 1;
  static const int max_hash_bits                  = BitsPerWord - age_bits - lock_bits - self_fwd_bits;
  static const int hash_bits                      = max_hash_bits > 31 ? 31 : max_hash_bits;
  static const int unused_gap_bits                = LP64_ONLY(4) NOT_LP64(0); // Reserved for Valhalla.
  static const int hashctrl_bits                  = 2;

  static const int lock_shift                     = 0;
  static const int self_fwd_shift                 = lock_shift + lock_bits;
  static const int age_shift                      = self_fwd_shift + self_fwd_bits;
  static const int hash_shift                     = age_shift + age_bits + unused_gap_bits;
  static const int hashctrl_shift                 = age_shift + age_bits + unused_gap_bits;

  static const uintptr_t lock_mask                = right_n_bits(lock_bits);
  static const uintptr_t lock_mask_in_place       = lock_mask << lock_shift;
  static const uintptr_t self_fwd_mask            = right_n_bits(self_fwd_bits);
  static const uintptr_t self_fwd_mask_in_place   = self_fwd_mask << self_fwd_shift;
  static const uintptr_t age_mask                 = right_n_bits(age_bits);
  static const uintptr_t age_mask_in_place        = age_mask << age_shift;
  static const uintptr_t hash_mask                = right_n_bits(hash_bits);
  static const uintptr_t hash_mask_in_place       = hash_mask << hash_shift;
  static const uintptr_t hashctrl_mask            = right_n_bits(hashctrl_bits);
  static const uintptr_t hashctrl_mask_in_place   = hashctrl_mask << hashctrl_shift;
  static const uintptr_t hashctrl_hashed_mask_in_place    = ((uintptr_t)1) << hashctrl_shift;
  static const uintptr_t hashctrl_expanded_mask_in_place  = ((uintptr_t)2) << hashctrl_shift;

#ifdef _LP64
  // Used only with compact headers:
  // We store the (narrow) Klass* in the bits 43 to 64.

  // These are for bit-precise extraction of the narrow Klass* from the 64-bit Markword
  static constexpr int klass_shift                = hashctrl_shift + hashctrl_bits;
  static constexpr int klass_bits                 = 19;
  static constexpr uintptr_t klass_mask           = right_n_bits(klass_bits);
  static constexpr uintptr_t klass_mask_in_place  = klass_mask << klass_shift;
#endif


  static const uintptr_t locked_value             = 0;
  static const uintptr_t unlocked_value           = 1;
  static const uintptr_t monitor_value            = 2;
  static const uintptr_t marked_value             = 3;
  static const uintptr_t forward_expanded_value   = 0b111;

  static const uintptr_t no_hash                  = 0 ;  // no hash value assigned
  static const uintptr_t no_hash_in_place         = (uintptr_t)no_hash << hash_shift;
  static const uintptr_t no_lock_in_place         = unlocked_value;

  static const uint max_age                       = age_mask;

  // Creates a markWord with all bits set to zero.
  static markWord zero() { return markWord(uintptr_t(0)); }

  // lock accessors (note that these assume lock_shift == 0)
  bool is_locked()   const {
    return (mask_bits(value(), lock_mask_in_place) != unlocked_value);
  }
  bool is_unlocked() const {
    return (mask_bits(value(), lock_mask_in_place) == unlocked_value);
  }
  bool is_marked()   const {
    return (value() & (self_fwd_mask_in_place | lock_mask_in_place)) > monitor_value;
  }
  bool is_forwarded() const {
    // Returns true for normal forwarded (0b011) and self-forwarded (0b1xx).
    return mask_bits(value(), lock_mask_in_place | self_fwd_mask_in_place) >= static_cast<intptr_t>(marked_value);
  }
  bool is_neutral()  const {  // Not locked, or marked - a "clean" neutral state
    return (mask_bits(value(), lock_mask_in_place) == unlocked_value);
  }

  markWord set_forward_expanded() {
    assert((value() & (lock_mask_in_place | self_fwd_mask_in_place)) == marked_value, "must be normal-forwarded here");
    return markWord(value() | forward_expanded_value);
  }
  bool is_forward_expanded() {
    return (value() & (lock_mask_in_place | self_fwd_mask_in_place)) == forward_expanded_value;
  }

  // Special temporary state of the markWord while being inflated.
  // Code that looks at mark outside a lock need to take this into account.
  bool is_being_inflated() const { return (value() == 0); }

  // Distinguished markword value - used when inflating over
  // an existing stack-lock.  0 indicates the markword is "BUSY".
  // Lockword mutators that use a LD...CAS idiom should always
  // check for and avoid overwriting a 0 value installed by some
  // other thread.  (They should spin or block instead.  The 0 value
  // is transient and *should* be short-lived).
  // Fast-locking does not use INFLATING.
  static markWord INFLATING() { return zero(); }    // inflate-in-progress

  // Should this header be preserved during GC?
  bool must_be_preserved() const {
    return UseCompactObjectHeaders ? !is_unlocked() : (!is_unlocked() || !has_no_hash());
  }

  // WARNING: The following routines are used EXCLUSIVELY by
  // synchronization functions. They are not really gc safe.
  // They must get updated if markWord layout get changed.
  markWord set_unlocked() const {
    return markWord(value() | unlocked_value);
  }
  bool has_locker() const {
    assert(LockingMode == LM_LEGACY, "should only be called with legacy stack locking");
    return (value() & lock_mask_in_place) == locked_value;
  }
  BasicLock* locker() const {
    assert(has_locker(), "check");
    return (BasicLock*) value();
  }

  bool is_fast_locked() const {
    assert(LockingMode == LM_LIGHTWEIGHT, "should only be called with new lightweight locking");
    return (value() & lock_mask_in_place) == locked_value;
  }
  markWord set_fast_locked() const {
    // Clear the lock_mask_in_place bits to set locked_value:
    return markWord(value() & ~lock_mask_in_place);
  }

  bool has_monitor() const {
    return ((value() & lock_mask_in_place) == monitor_value);
  }
  ObjectMonitor* monitor() const {
    assert(has_monitor(), "check");
    assert(!UseObjectMonitorTable, "Lightweight locking with OM table does not use markWord for monitors");
    // Use xor instead of &~ to provide one extra tag-bit check.
    return (ObjectMonitor*) (value() ^ monitor_value);
  }
  bool has_displaced_mark_helper() const {
    intptr_t lockbits = value() & lock_mask_in_place;
    if (LockingMode == LM_LIGHTWEIGHT) {
      return !UseObjectMonitorTable && lockbits == monitor_value;
    }
    // monitor (0b10) | stack-locked (0b00)?
    return (lockbits & unlocked_value) == 0;
  }
  markWord displaced_mark_helper() const;
  void set_displaced_mark_helper(markWord m) const;
  markWord copy_set_hash(intptr_t hash) const {
    assert(!UseCompactObjectHeaders, "Do not use with compact i-hash");
    uintptr_t tmp = value() & (~hash_mask_in_place);
    tmp |= ((hash & hash_mask) << hash_shift);
    return markWord(tmp);
  }
  // it is only used to be stored into BasicLock as the
  // indicator that the lock is using heavyweight monitor
  static markWord unused_mark() {
    return markWord(marked_value);
  }
  // the following two functions create the markWord to be
  // stored into object header, it encodes monitor info
  static markWord encode(BasicLock* lock) {
    return from_pointer(lock);
  }
  static markWord encode(ObjectMonitor* monitor) {
    assert(!UseObjectMonitorTable, "Lightweight locking with OM table does not use markWord for monitors");
    uintptr_t tmp = (uintptr_t) monitor;
    return markWord(tmp | monitor_value);
  }

  markWord set_has_monitor() const {
    return markWord((value() & ~lock_mask_in_place) | monitor_value);
  }

  // used to encode pointers during GC
  markWord clear_lock_bits() const { return markWord(value() & ~(lock_mask_in_place | self_fwd_mask_in_place)); }

  // age operations
  markWord set_marked()   { return markWord((value() & ~lock_mask_in_place) | marked_value); }
  markWord set_unmarked() { return markWord((value() & ~lock_mask_in_place) | unlocked_value); }

  uint     age()           const { return (uint) mask_bits(value() >> age_shift, age_mask); }
  markWord set_age(uint v) const {
    assert((v & ~age_mask) == 0, "shouldn't overflow age field");
    return markWord((value() & ~age_mask_in_place) | ((v & age_mask) << age_shift));
  }
  markWord incr_age()      const { return age() == max_age ? markWord(_value) : set_age(age() + 1); }

  // hash operations
  intptr_t hash() const {
    assert(!UseCompactObjectHeaders, "only without compact i-hash");
    return mask_bits(value() >> hash_shift, hash_mask);
  }

  bool has_no_hash() const {
    if (UseCompactObjectHeaders) {
      return !is_hashed();
    } else {
      return hash() == no_hash;
    }
  }

  inline bool is_hashed_not_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return (value() & hashctrl_mask_in_place) == hashctrl_hashed_mask_in_place;
  }
  inline markWord set_hashed_not_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return markWord((value() & ~hashctrl_mask_in_place) | hashctrl_hashed_mask_in_place);
  }

  inline bool is_hashed_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return (value() & hashctrl_mask_in_place) == (hashctrl_hashed_mask_in_place | hashctrl_expanded_mask_in_place);
  }
  inline markWord set_hashed_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return markWord((value() & ~hashctrl_mask_in_place) | (hashctrl_hashed_mask_in_place | hashctrl_expanded_mask_in_place));
  }

  // This is a special hashctrl state (11) that is only used
  // during CDS archive dumping. There we allocate 'scratch mirrors' for
  // each real mirror klass. We allocate those scratch mirrors
  // in a pre-extended form, but without being hashed. When the
  // real mirror gets hashed, then we turn the scratch mirror into
  // hashed_moved state, otherwise we leave it in that special state
  // which indicates that the archived copy will be allocated in the
  // unhashed form.
  inline bool is_not_hashed_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return (value() & hashctrl_mask_in_place) == hashctrl_expanded_mask_in_place;
  }
  inline markWord set_not_hashed_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return markWord((value() & ~hashctrl_mask_in_place) | hashctrl_expanded_mask_in_place);
  }
  // Return true when object is either hashed_moved or not_hashed_moved.
  inline bool is_expanded() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return (value() & hashctrl_expanded_mask_in_place) != 0;
  }
  inline bool is_hashed() const {
    assert(UseCompactObjectHeaders, "only with compact i-hash");
    return (value() & hashctrl_hashed_mask_in_place) != 0;
  }

  inline markWord copy_hashctrl_from(markWord m) const {
    if (UseCompactObjectHeaders) {
      return markWord((value() & ~hashctrl_mask_in_place) | (m.value() & hashctrl_mask_in_place));
    } else {
      return markWord(value());
    }
  }

  inline Klass* klass() const;
  inline Klass* klass_or_null() const;
  inline Klass* klass_without_asserts() const;
  inline narrowKlass narrow_klass() const;
  inline markWord set_narrow_klass(narrowKlass narrow_klass) const;

#ifdef _LP64
  inline int array_length() { return checked_cast<int>(value() >> 32); }
#endif

  // Prototype mark for initialization
  static markWord prototype() {
    if (UseCompactObjectHeaders) {
      return markWord(no_lock_in_place);
    } else {
      return markWord(no_hash_in_place | no_lock_in_place);
    }
  }

  // Debugging
  void print_on(outputStream* st, bool print_monitor_info = true) const;

  // Prepare address of oop for placement into mark
  inline static markWord encode_pointer_as_mark(void* p) { return from_pointer(p).set_marked(); }

  // Recover address of oop from encoded form used in mark
  inline void* decode_pointer() const { return (void*)clear_lock_bits().value(); }

  inline bool is_self_forwarded() const {
    NOT_LP64(assert(LockingMode != LM_LEGACY, "incorrect with LM_LEGACY on 32 bit");)
    // Match 100, 101, 110 but not 111.
    return mask_bits(value() + 1, (lock_mask_in_place | self_fwd_mask_in_place)) > 4;
  }

  inline markWord set_self_forwarded() const {
    NOT_LP64(assert(LockingMode != LM_LEGACY, "incorrect with LM_LEGACY on 32 bit");)
    return markWord(value() | self_fwd_mask_in_place);
  }

  inline markWord unset_self_forwarded() const {
    NOT_LP64(assert(LockingMode != LM_LEGACY, "incorrect with LM_LEGACY on 32 bit");)
    return markWord(value() & ~self_fwd_mask_in_place);
  }

  inline oop forwardee() const {
    return cast_to_oop(decode_pointer());
  }
};

// Support atomic operations.
template<>
struct PrimitiveConversions::Translate<markWord> : public std::true_type {
  typedef markWord Value;
  typedef uintptr_t Decayed;

  static Decayed decay(const Value& x) { return x.value(); }
  static Value recover(Decayed x) { return Value(x); }
};

#endif // SHARE_OOPS_MARKWORD_HPP
