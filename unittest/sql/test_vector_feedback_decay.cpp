/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "sql/das/iter/vector_feedback_decay.h"

namespace oceanbase
{
namespace sql
{

TEST(TestVectorFeedbackDecay, UnusedPathStaysZero)
{
  EXPECT_EQ(0, decay_vector_path_count(0, 0.5));
}

TEST(TestVectorFeedbackDecay, CorruptedNegativeCountIsDiscarded)
{
  EXPECT_EQ(0, decay_vector_path_count(-1, 0.5));
  EXPECT_EQ(0, decay_vector_path_count(std::numeric_limits<int64_t>::min(), 0.5));
}

TEST(TestVectorFeedbackDecay, PositiveCountRetainsExistingFormula)
{
  const int64_t counts[] = {1, 2, 7, 8, 29, 30, 1000, 1000000,
                            std::numeric_limits<int64_t>::max()};
  for (const int64_t count : counts) {
    EXPECT_EQ(static_cast<int64_t>(std::log(static_cast<double>(count)) * 0.5),
              decay_vector_path_count(count, 0.5)) << count;
  }
}

TEST(TestVectorFeedbackDecay, RepeatedDecayKeepsUnusedPathsValid)
{
  // Simulate the counts supplied by successive feedback windows. The active
  // path may change, while the other two paths can retain zero counts.
  std::array<int64_t, 3> counts = {{0, 0, 0}};
  for (int window = 0; window < 300; ++window) {
    const int active = window / 100;
    counts[active] += 29;
    for (int path = 0; path < 3; ++path) {
      counts[path] = decay_vector_path_count(counts[path], 0.5);
      EXPECT_GE(counts[path], 0);
      EXPECT_LE(counts[path], 2);
    }
    EXPECT_EQ(1, counts[active]);
  }
}

} // namespace sql
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
