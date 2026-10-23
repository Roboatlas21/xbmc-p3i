/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "filesystem/CacheStrategy.h"
#include "filesystem/CircularCache.h"

#include <gtest/gtest.h>

#include <limits>

#if defined(TARGET_LINUX)
TEST(TestCircularCache, UnallocatableSizeReturnsError)
{
  // Verify that a failed allocation is logged and returned, not thrown up
  // through File::Open as an anonymous exception.
  XFILE::CCircularCache cache(std::numeric_limits<size_t>::max(), 0);
  EXPECT_EQ(CACHE_RC_ERROR, cache.Open());
  cache.Close();
}
#endif
