// Rolling statistics of the built-in profiler (R4.2): nearest-rank
// percentiles, the sample ring's wraparound, empty states.

#include <gtest/gtest.h>

#include <numeric>
#include <vector>

#include "engine/core/profile_stats.hpp"

namespace {

using engine::core::compute_stats;
using engine::core::percentile;
using engine::core::SampleRing;

TEST(ProfileStats, PercentileIsNearestRank) {
    std::vector<float> v(100);
    std::iota(v.begin(), v.end(), 1.0F); // 1..100
    EXPECT_FLOAT_EQ(percentile(v, 95.0F), 95.0F);
    EXPECT_FLOAT_EQ(percentile(v, 50.0F), 50.0F);
    EXPECT_FLOAT_EQ(percentile(v, 100.0F), 100.0F);
    EXPECT_FLOAT_EQ(percentile(v, 0.0F), 1.0F); // the minimum
    EXPECT_FLOAT_EQ(percentile(v, 0.5F), 1.0F);

    // Small sets round the rank up: p50 of four samples is the second.
    std::vector<float> four{4.0F, 1.0F, 3.0F, 2.0F};
    EXPECT_FLOAT_EQ(percentile(four, 50.0F), 2.0F);
    EXPECT_FLOAT_EQ(percentile(four, 95.0F), 4.0F);
    // Out-of-range p is clamped.
    EXPECT_FLOAT_EQ(percentile(four, 250.0F), 4.0F);
    EXPECT_FLOAT_EQ(percentile(four, -3.0F), 1.0F);
}

TEST(ProfileStats, PercentileOfOneSampleIsThatSample) {
    std::vector<float> one{7.5F};
    EXPECT_FLOAT_EQ(percentile(one, 50.0F), 7.5F);
    EXPECT_FLOAT_EQ(percentile(one, 95.0F), 7.5F);
}

TEST(ProfileStats, EmptyInputsAreZero) {
    std::vector<float> none;
    EXPECT_FLOAT_EQ(percentile(none, 95.0F), 0.0F);
    const auto s = compute_stats(none);
    EXPECT_EQ(s.count, 0U);
    EXPECT_FLOAT_EQ(s.avg, 0.0F);
    EXPECT_FLOAT_EQ(s.max, 0.0F);

    const SampleRing ring(8);
    EXPECT_TRUE(ring.empty());
    EXPECT_FLOAT_EQ(ring.latest(), 0.0F);
    EXPECT_EQ(ring.stats().count, 0U);

    SampleRing unsized; // capacity 0 ignores pushes
    unsized.push(3.0F);
    EXPECT_TRUE(unsized.empty());
}

TEST(ProfileStats, ComputeStatsSummarises) {
    std::vector<float> v{2.0F, 4.0F, 6.0F, 8.0F, 30.0F};
    const auto s = compute_stats(v);
    EXPECT_EQ(s.count, 5U);
    EXPECT_FLOAT_EQ(s.avg, 10.0F);
    EXPECT_FLOAT_EQ(s.p50, 6.0F);
    EXPECT_FLOAT_EQ(s.p95, 30.0F);
    EXPECT_FLOAT_EQ(s.max, 30.0F);
}

TEST(ProfileStats, RingKeepsTheLastCapacitySamplesInOrder) {
    SampleRing ring(4);
    for (int i = 1; i <= 3; ++i) ring.push(static_cast<float>(i));
    EXPECT_EQ(ring.size(), 3U);
    EXPECT_FLOAT_EQ(ring.at(0), 1.0F);
    EXPECT_FLOAT_EQ(ring.latest(), 3.0F);

    for (int i = 4; i <= 10; ++i) ring.push(static_cast<float>(i)); // wraps twice
    EXPECT_EQ(ring.size(), 4U);
    std::vector<float> out;
    ring.copy_to(out);
    EXPECT_EQ(out, (std::vector<float>{7.0F, 8.0F, 9.0F, 10.0F}));
    const auto s = ring.stats();
    EXPECT_EQ(s.count, 4U);
    EXPECT_FLOAT_EQ(s.avg, 8.5F);
    EXPECT_FLOAT_EQ(s.max, 10.0F);
    EXPECT_FLOAT_EQ(s.last, 10.0F);

    ring.clear();
    EXPECT_TRUE(ring.empty());
    ring.push(5.0F);
    EXPECT_FLOAT_EQ(ring.at(0), 5.0F);
}

TEST(ProfileStats, RingWrapsExactlyAtCapacity) {
    SampleRing ring(3);
    for (int i = 0; i < 3; ++i) ring.push(static_cast<float>(i));
    EXPECT_FLOAT_EQ(ring.at(0), 0.0F);
    EXPECT_FLOAT_EQ(ring.at(2), 2.0F);
    ring.push(3.0F);
    EXPECT_FLOAT_EQ(ring.at(0), 1.0F);
    EXPECT_FLOAT_EQ(ring.latest(), 3.0F);
}

} // namespace
