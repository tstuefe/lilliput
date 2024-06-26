/*
 * Copyright (c) 2017, 2023, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_OOPS_COMPRESSEDKLASS_INLINE_HPP
#define SHARE_OOPS_COMPRESSEDKLASS_INLINE_HPP

#include "oops/compressedKlass.hpp"

#include "memory/universe.hpp"
#include "oops/oop.hpp"
#include "utilities/align.hpp"
#include "utilities/globalDefinitions.hpp"

inline Klass* CompressedKlassPointers::decode_not_null_without_asserts(narrowKlass v, address narrow_base_base, int shift) {
  return (Klass*)((uintptr_t)narrow_base_base +((uintptr_t)v << shift));
}

inline Klass* CompressedKlassPointers::decode_not_null(narrowKlass v, address narrow_base, int shift) {
  assert(!is_null(v), "narrow klass value can never be zero");
  Klass* result = decode_not_null_without_asserts(v, narrow_base, shift);
  DEBUG_ONLY(check_valid_klass(result, narrow_base, shift));
  return result;
}

inline narrowKlass CompressedKlassPointers::encode_not_null_without_asserts(Klass* k, address narrow_base, int shift) {
  return (narrowKlass)(pointer_delta(k, narrow_base, 1) >> shift);
}

inline narrowKlass CompressedKlassPointers::encode_not_null(Klass* v, address narrow_base, int shift) {
  assert(!is_null(v), "klass value can never be zero");
  DEBUG_ONLY(check_valid_klass(v);)
  narrowKlass result = encode_not_null_without_asserts(v, narrow_base, shift);
  assert(decode_not_null((narrowKlass)result, narrow_base, shift) == v, "reversibility");
  return result;
}

inline Klass* CompressedKlassPointers::decode_not_null_without_asserts(narrowKlass v) {
  return decode_not_null_without_asserts(v, base(), shift());
}

inline Klass* CompressedKlassPointers::decode_without_asserts(narrowKlass v) {
  return is_null(v) ? nullptr : decode_not_null_without_asserts(v);
}

inline Klass* CompressedKlassPointers::decode_not_null(narrowKlass v) {
  DEBUG_ONLY(check_valid_narrow_klass_id(v);)
  return decode_not_null(v, base(), shift());
}

inline Klass* CompressedKlassPointers::decode(narrowKlass v) {
  return is_null(v) ? nullptr : decode_not_null(v);
}

inline narrowKlass CompressedKlassPointers::encode_not_null(Klass* v) {
  narrowKlass nk = encode_not_null(v, base(), shift());
  DEBUG_ONLY(check_valid_narrow_klass_id(nk);)
  return nk;
}

inline narrowKlass CompressedKlassPointers::encode(Klass* v) {
  return is_null(v) ? (narrowKlass)0 : encode_not_null(v);
}

#ifdef ASSERT
inline void CompressedKlassPointers::check_valid_klass(const Klass* k, address base, int shift) {
  const int log_alignment = MAX2(3, shift); // always at least 64-bit aligned
  assert(is_aligned(k, nth_bit(log_alignment)), "Klass (" PTR_FORMAT ") not properly aligned to %zu",
         p2i(k), nth_bit(shift));
  const address encoding_end = base + nth_bit(narrow_klass_pointer_bits() + shift);
  assert((address)k >= base && (address)k < encoding_end,
         "Klass (" PTR_FORMAT ") falls outside of the valid encoding range [" PTR_FORMAT "-" PTR_FORMAT ")",
         p2i(k), p2i(base), p2i(encoding_end));
}

inline void CompressedKlassPointers::check_valid_klass(const Klass* k) {
  assert(UseCompressedClassPointers, "Only call for +UseCCP");
  check_valid_klass(k, base(), shift());
  // Also assert that k falls into what we know is the valid Klass range. This is usually smaller
  // than the encoding range (e.g. encoding range covers 4G, but we only have 1G class space and a
  // tiny bit of CDS => 1.1G)
  const address klassrange_end = base() + range();
  assert((address)k < klassrange_end,
      "Klass (" PTR_FORMAT ") falls outside of the valid klass range [" PTR_FORMAT "-" PTR_FORMAT ")",
      p2i(k), p2i(base()), p2i(klassrange_end));
}
inline void CompressedKlassPointers::check_valid_narrow_klass_id(narrowKlass nk) {
  assert(UseCompressedClassPointers, "Only call for +UseCCP");
  const uint64_t nk_mask = ~right_n_bits(narrow_klass_pointer_bits());
  assert(((uint64_t)nk & nk_mask) == 0, "narrow klass id bit spillover (%u)", nk);
  assert(nk >= _lowest_valid_narrow_klass_id &&
         nk <= _highest_valid_narrow_klass_id, "narrowKlass ID out of range (%u)", nk);
}
#endif // ASSERT

#endif // SHARE_OOPS_COMPRESSEDKLASS_INLINE_HPP
