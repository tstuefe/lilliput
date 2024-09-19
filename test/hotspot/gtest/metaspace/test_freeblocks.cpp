/*
 * Copyright (c) 2020, 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "memory/metaspace/counters.hpp"
#include "memory/metaspace/freeBlocks.hpp"
#include "memory/metaspace/metablock.hpp"

//#define LOG_PLEASE
#include "metaspaceGtestCommon.hpp"

// FreeBlocks is just a wrapper around a BinList and a BlockTree. More extensive tests
// are done in the gtests for these two sub constructs. Here, we just test basic functionality.

using metaspace::FreeBlocks;
using metaspace::MetaBlock;
using metaspace::SizeCounter;

#define CHECK_CONTENT(fb, num_blocks_expected, word_size_expected) \
{ \
  if (word_size_expected > 0) { \
    EXPECT_FALSE(fb.is_empty()); \
  } else { \
    EXPECT_TRUE(fb.is_empty()); \
  } \
  EXPECT_EQ(fb.total_size(), (size_t)word_size_expected); \
  EXPECT_EQ(fb.count(), (int)num_blocks_expected); \
}

static void add_one_block_and_test(FreeBlocks& fb, MetaBlock blk) {
  const size_t size_0 = fb.total_size();
  const int count_0 = fb.count();
  fb.add_block(blk);
  DEBUG_ONLY(fb.verify();)
  ASSERT_FALSE(fb.is_empty());
  CHECK_CONTENT(fb, count_0 + 1, size_0 + blk.word_size());
}

static MetaBlock remove_one_block_and_test(FreeBlocks& fb, size_t word_size) {
  const size_t size_0 = fb.total_size();
  const int count_0 = fb.count();
  MetaBlock blk = fb.remove_block(word_size);
  if (blk.is_empty()) {
    CHECK_CONTENT(fb, count_0, size_0);
  } else {
    CHECK_CONTENT(fb, count_0 - 1, size_0 - blk.word_size());
    EXPECT_GE(blk.word_size(), word_size);
  }
  return blk;
}

TEST_VM(metaspace, freeblocks_basics) {

  FreeBlocks fbl;
  CHECK_CONTENT(fbl, 0, 0);

  constexpr size_t tmpbufsize = 1024 * 3;
  MetaWord tmp[tmpbufsize];

  MetaWord* p = tmp;
  MetaBlock b16  (p, 16);
  p += b16.word_size();

  MetaBlock b256 (p, 256);
  p += b256.word_size();

  MetaBlock b1024 (p, 1024);
  p += b1024.word_size();
  assert(p <= tmp + tmpbufsize, "increase temp buffer size");

  add_one_block_and_test(fbl, b16);
  CHECK_CONTENT(fbl, 1, 16);

  MetaBlock b = remove_one_block_and_test(fbl, 256); // too large
  ASSERT_TRUE(b.is_empty());

  b = remove_one_block_and_test(fbl, 8); // smaller - will return block
  ASSERT_EQ(b, b16);
  CHECK_CONTENT(fbl, 0, 0); // empty now

  add_one_block_and_test(fbl, b16);
  CHECK_CONTENT(fbl, 1, 16);

  add_one_block_and_test(fbl, b1024);
  CHECK_CONTENT(fbl, 2, 16 + 1024);

  add_one_block_and_test(fbl, b256);
  CHECK_CONTENT(fbl, 3, 16 + 1024 + 256);

  b = remove_one_block_and_test(fbl, 1024 + 1); // too large
  ASSERT_TRUE(b.is_empty());

  b = remove_one_block_and_test(fbl, 256); // Should return the 256 block
  ASSERT_EQ(b, b256) << b.word_size();
  CHECK_CONTENT(fbl, 2, 16 + 1024);

  b = remove_one_block_and_test(fbl, 256); // Should return the 1024 block
  ASSERT_EQ(b, b1024);
  CHECK_CONTENT(fbl, 1, 16);

  b = remove_one_block_and_test(fbl, 256); // Should fail
  ASSERT_TRUE(b.is_empty());

  b = remove_one_block_and_test(fbl, 8); // Should return the 16 block
  ASSERT_EQ(b, b16);
  CHECK_CONTENT(fbl, 0, 0); // empty now

}
