/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "filesystem/CacheStrategy.h"
#include "filesystem/CircularCache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace XFILE;

namespace
{
constexpr size_t SEGMENT_SIZE = 8 * 1024 * 1024;

void WriteAll(CCircularCache& cache, const std::vector<char>& data)
{
  size_t written = 0;
  while (written < data.size())
  {
    const int bytes = cache.WriteToCache(data.data() + written, data.size() - written);
    ASSERT_GT(bytes, 0);
    written += bytes;
  }
}

void ReadAll(CCircularCache& cache, std::vector<char>& data)
{
  size_t read = 0;
  while (read < data.size())
  {
    const int bytes = cache.ReadFromCache(data.data() + read, data.size() - read);
    ASSERT_GT(bytes, 0);
    read += bytes;
  }
}
} // namespace

TEST(TestCircularCache, CrossesSegmentBoundary)
{
  CCircularCache cache(SEGMENT_SIZE + 256, 256);
  ASSERT_EQ(CACHE_RC_OK, cache.Open());

  std::vector<char> input(SEGMENT_SIZE + 128);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<char>(i % 251);
  WriteAll(cache, input);

  std::vector<char> output(input.size());
  ReadAll(cache, output);
  EXPECT_EQ(input, output);
}

TEST(TestCircularCache, WrapsAfterLastSegment)
{
  CCircularCache cache(SEGMENT_SIZE + 64, 64);
  ASSERT_EQ(CACHE_RC_OK, cache.Open());

  std::vector<char> first(SEGMENT_SIZE + 64, 'a');
  WriteAll(cache, first);
  std::vector<char> consumed(first.size());
  ReadAll(cache, consumed);
  EXPECT_EQ(first, consumed);

  // 64 bytes fit at the physical tail; the rest wraps to the start.
  std::vector<char> second(128);
  for (size_t i = 0; i < second.size(); ++i)
    second[i] = static_cast<char>(i);
  WriteAll(cache, second);
  std::vector<char> output(second.size());
  ReadAll(cache, output);
  EXPECT_EQ(second, output);
}

TEST(TestCircularCache, SeeksAcrossSegmentsAndResetsAtNonzeroOffset)
{
  CCircularCache cache(SEGMENT_SIZE + 512, 512);
  ASSERT_EQ(CACHE_RC_OK, cache.Open());
  std::vector<char> input(SEGMENT_SIZE + 128);
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<char>((i * 13) % 253);
  WriteAll(cache, input);

  const int64_t seekTo = SEGMENT_SIZE - 64;
  ASSERT_EQ(seekTo, cache.Seek(seekTo));
  std::vector<char> crossing(192);
  ReadAll(cache, crossing);
  EXPECT_TRUE(std::equal(crossing.begin(), crossing.end(), input.begin() + seekTo));

  const int64_t farOffset = 30000000;
  EXPECT_TRUE(cache.Reset(farOffset));
  EXPECT_EQ(farOffset, cache.CachedDataStartPos());
  EXPECT_EQ(farOffset, cache.CachedDataEndPos());

  std::vector<char> later(1024, 'z');
  WriteAll(cache, later);
  ASSERT_EQ(farOffset, cache.Seek(farOffset));
  std::vector<char> output(later.size());
  ReadAll(cache, output);
  EXPECT_EQ(later, output);
}

TEST(TestCircularCache, DoubleCacheCanSwapBackToEarlierData)
{
  CDoubleCache cache(new CCircularCache(SEGMENT_SIZE + 256, 256));
  ASSERT_EQ(CACHE_RC_OK, cache.Open());

  std::vector<char> first(SEGMENT_SIZE + 128, 'A');
  size_t written = 0;
  while (written < first.size())
  {
    const int bytes = cache.WriteToCache(first.data() + written, first.size() - written);
    ASSERT_GT(bytes, 0);
    written += bytes;
  }

  EXPECT_TRUE(cache.Reset(100000000));
  const std::vector<char> second(128, 'B');
  EXPECT_EQ(static_cast<int>(second.size()), cache.WriteToCache(second.data(), second.size()));
  EXPECT_FALSE(cache.Reset(0));

  std::vector<char> output(first.size());
  size_t read = 0;
  while (read < output.size())
  {
    const int bytes = cache.ReadFromCache(output.data() + read, output.size() - read);
    ASSERT_GT(bytes, 0);
    read += bytes;
  }
  EXPECT_EQ(first, output);
}

TEST(TestCircularCache, CloseAllowsCleanReopen)
{
  CCircularCache cache(SEGMENT_SIZE + 256, 256);
  ASSERT_EQ(CACHE_RC_OK, cache.Open());
  EXPECT_EQ(1, cache.WriteToCache("x", 1));
  cache.Close();

  ASSERT_EQ(CACHE_RC_OK, cache.Open());
  EXPECT_EQ(0, cache.CachedDataEndPos());
  EXPECT_EQ(1, cache.WriteToCache("y", 1));
  char got = 0;
  EXPECT_EQ(1, cache.ReadFromCache(&got, 1));
  EXPECT_EQ('y', got);
}

#if !defined(TARGET_WINDOWS)
TEST(TestCircularCache, ZeroCapacityFailsWithoutAllocating)
{
  CCircularCache cache(0, 0);
  EXPECT_EQ(CACHE_RC_ERROR, cache.Open());
  cache.Close();
}
#endif
