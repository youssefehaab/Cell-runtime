#include "common/histogram.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace cell {
namespace {

constexpr double kPercentiles[] = {0.0,  1.0,  10.0, 25.0, 50.0,  75.0,
                                   90.0, 95.0, 99.0, 99.9, 99.99, 100.0};
constexpr std::uint32_t kSeed = 0xC311'0001;

// Nearest-rank percentile on the raw samples: the ground truth the histogram approximates.
std::uint64_t reference_percentile(const std::vector<std::uint64_t>& sorted, double p) {
  const auto n = static_cast<double>(sorted.size());
  const auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * n));
  return sorted[std::clamp<std::size_t>(rank, 1, sorted.size()) - 1];
}

// Records `samples` and checks every percentile against the documented bound:
// true <= reported <= true * (1 + 2^-bits), and reported <= max.
void expect_percentiles_within_bound(std::vector<std::uint64_t> samples, unsigned bits) {
  LatencyHistogram h(bits);
  for (const auto v : samples) h.record(v);
  std::sort(samples.begin(), samples.end());

  const double rel = std::ldexp(1.0, -static_cast<int>(bits));
  for (const double p : kPercentiles) {
    const std::uint64_t truth = reference_percentile(samples, p);
    const std::uint64_t got = h.percentile(p);
    EXPECT_GE(got, truth) << "p=" << p << " bits=" << bits;
    EXPECT_LE(static_cast<double>(got), static_cast<double>(truth) * (1.0 + rel))
        << "p=" << p << " bits=" << bits << " truth=" << truth;
    EXPECT_LE(got, h.max()) << "p=" << p;
    EXPECT_GE(got, h.min()) << "p=" << p;
  }
  EXPECT_EQ(h.percentile(0.0), samples.front());
  EXPECT_EQ(h.percentile(100.0), samples.back());
}

TEST(LatencyHistogram, EmptyReportsZeros) {
  const LatencyHistogram h;
  EXPECT_EQ(h.count(), 0U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 0U);
  EXPECT_EQ(h.mean(), 0.0);
  EXPECT_EQ(h.percentile(0.0), 0U);
  EXPECT_EQ(h.percentile(50.0), 0U);
  EXPECT_EQ(h.percentile(100.0), 0U);
}

TEST(LatencyHistogram, BucketCountMatchesDocumentedLayout) {
  EXPECT_EQ(LatencyHistogram(5).bucket_count(), 60U * 32U);
  EXPECT_EQ(LatencyHistogram(0).bucket_count(), 65U);
  EXPECT_EQ(LatencyHistogram(8).bucket_count(), 57U * 256U);
  EXPECT_THROW(LatencyHistogram(LatencyHistogram::kMaxSubBucketBits + 1), std::invalid_argument);
}

TEST(LatencyHistogram, SmallValuesAreExact) {
  constexpr unsigned kBits = 5;
  constexpr std::uint64_t kExactLimit = std::uint64_t{1} << kBits;  // 32
  LatencyHistogram h(kBits);
  for (std::uint64_t v = 0; v < kExactLimit; ++v) h.record(v);

  // One sample per value, so the k-th smallest sample is exactly k-1. Querying mid-rank
  // (k - 0.5) keeps ceil() away from floating-point rounding at the rank boundaries.
  for (std::uint64_t k = 1; k <= kExactLimit; ++k) {
    const double p = 100.0 * (static_cast<double>(k) - 0.5) / static_cast<double>(kExactLimit);
    EXPECT_EQ(h.percentile(p), k - 1) << "p=" << p;
  }
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), kExactLimit - 1);
  EXPECT_DOUBLE_EQ(h.mean(), static_cast<double>(kExactLimit - 1) / 2.0);
}

TEST(LatencyHistogram, MinMaxMeanCount) {
  LatencyHistogram h;
  for (const std::uint64_t v : {1'000U, 250U, 4'000'000U, 750U}) h.record(v);
  EXPECT_EQ(h.count(), 4U);
  EXPECT_EQ(h.min(), 250U);
  EXPECT_EQ(h.max(), 4'000'000U);
  EXPECT_DOUBLE_EQ(h.mean(), (1'000.0 + 250.0 + 4'000'000.0 + 750.0) / 4.0);
}

TEST(LatencyHistogram, RecordingZeroIsDistinctFromEmpty) {
  LatencyHistogram h;
  h.record(0);
  EXPECT_EQ(h.count(), 1U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 0U);
  EXPECT_EQ(h.percentile(50.0), 0U);
}

TEST(LatencyHistogram, UniformDistribution) {
  std::mt19937_64 rng(kSeed);
  std::uniform_int_distribution<std::uint64_t> dist(1'000, 5'000'000);  // 1 us .. 5 ms
  std::vector<std::uint64_t> samples(200'000);
  for (auto& v : samples) v = dist(rng);
  for (const unsigned bits : {3U, 5U, 7U}) expect_percentiles_within_bound(samples, bits);
}

TEST(LatencyHistogram, LogNormalDistribution) {
  // Heavy right tail spanning several decades, like real latency data.
  std::mt19937_64 rng(kSeed);
  std::lognormal_distribution<double> dist(std::log(50'000.0), 1.5);
  std::vector<std::uint64_t> samples(200'000);
  for (auto& v : samples) v = static_cast<std::uint64_t>(dist(rng));
  for (const unsigned bits : {3U, 5U, 7U}) expect_percentiles_within_bound(samples, bits);
}

TEST(LatencyHistogram, ConstantDistribution) {
  constexpr std::uint64_t kValue = 123'457;  // not a bucket boundary
  expect_percentiles_within_bound(std::vector<std::uint64_t>(10'000, kValue), 5);

  // Clamping to max makes a constant stream exact at every percentile.
  LatencyHistogram h;
  for (int i = 0; i < 10'000; ++i) h.record(kValue);
  for (const double p : kPercentiles) EXPECT_EQ(h.percentile(p), kValue) << "p=" << p;
}

TEST(LatencyHistogram, SingleOutlier) {
  constexpr std::uint64_t kTypical = 1'000;
  constexpr std::uint64_t kOutlier = 30'000'000'000;  // 30 s stall
  std::vector<std::uint64_t> samples(9'999, kTypical);
  samples.push_back(kOutlier);
  expect_percentiles_within_bound(samples, 5);

  LatencyHistogram h;
  for (const auto v : samples) h.record(v);
  // The outlier owns only the very last rank: it must not leak into p99.9 ...
  EXPECT_GE(h.percentile(99.9), kTypical);
  EXPECT_LE(h.percentile(99.9), kTypical + kTypical / 32);
  // ... and must be reported exactly at the top.
  EXPECT_EQ(h.percentile(100.0), kOutlier);
  EXPECT_EQ(h.max(), kOutlier);
}

TEST(LatencyHistogram, HugeValues) {
  LatencyHistogram h;
  h.record(UINT64_MAX);
  EXPECT_EQ(h.count(), 1U);
  EXPECT_EQ(h.min(), UINT64_MAX);
  EXPECT_EQ(h.max(), UINT64_MAX);
  EXPECT_EQ(h.percentile(50.0), UINT64_MAX);

  h.record(UINT64_MAX);
  h.record(UINT64_MAX - 1);
  h.record(std::uint64_t{1} << 63);
  h.record(1);
  EXPECT_EQ(h.count(), 5U);
  EXPECT_EQ(h.min(), 1U);
  EXPECT_EQ(h.max(), UINT64_MAX);
  EXPECT_EQ(h.percentile(100.0), UINT64_MAX);
  // The sum exceeds 2^64; the mean must not wrap.
  const double expected_mean = (3.0 * 18446744073709551615.0 + 9223372036854775808.0) / 5.0;
  EXPECT_NEAR(h.mean(), expected_mean, expected_mean * 1e-12);

  // Rank 2 of {1, 2^63, MAX-1, MAX, MAX} is 2^63; the bound still holds up here.
  const std::uint64_t p30 = h.percentile(30.0);
  EXPECT_GE(p30, std::uint64_t{1} << 63);
  EXPECT_LE(static_cast<double>(p30), 9223372036854775808.0 * (1.0 + 1.0 / 32.0));

  expect_percentiles_within_bound(
      {UINT64_MAX, UINT64_MAX - 1, UINT64_MAX / 2, UINT64_MAX / 3, 1, 0}, 5);
}

TEST(LatencyHistogram, PercentileArgumentIsClamped) {
  LatencyHistogram h;
  for (std::uint64_t v = 100; v <= 200; ++v) h.record(v);
  EXPECT_EQ(h.percentile(-5.0), 100U);
  EXPECT_EQ(h.percentile(250.0), 200U);
  EXPECT_EQ(h.percentile(std::nan("")), 100U);
}

TEST(LatencyHistogram, PercentileIsMonotonicInP) {
  std::mt19937_64 rng(kSeed);
  std::lognormal_distribution<double> dist(std::log(20'000.0), 1.0);
  LatencyHistogram h;
  for (int i = 0; i < 50'000; ++i) h.record(static_cast<std::uint64_t>(dist(rng)));
  std::uint64_t prev = 0;
  for (double p = 0.0; p <= 100.0; p += 0.25) {
    const std::uint64_t v = h.percentile(p);
    EXPECT_GE(v, prev) << "p=" << p;
    prev = v;
  }
}

TEST(LatencyHistogram, ResetClearsEverything) {
  LatencyHistogram h;
  for (std::uint64_t v = 1; v <= 1'000; ++v) h.record(v * 1'000);
  h.record(UINT64_MAX);
  h.reset();
  EXPECT_EQ(h.count(), 0U);
  EXPECT_EQ(h.min(), 0U);
  EXPECT_EQ(h.max(), 0U);
  EXPECT_EQ(h.mean(), 0.0);
  EXPECT_EQ(h.percentile(99.0), 0U);

  // Usable again, with no residue from before the reset.
  h.record(42);
  EXPECT_EQ(h.count(), 1U);
  EXPECT_EQ(h.min(), 42U);
  EXPECT_EQ(h.max(), 42U);
  EXPECT_DOUBLE_EQ(h.mean(), 42.0);
  EXPECT_EQ(h.percentile(50.0), 42U);
}

TEST(LatencyHistogram, MergeEqualsRecordingEverythingIntoOne) {
  std::mt19937_64 rng(kSeed);
  std::uniform_int_distribution<std::uint64_t> fast(500, 20'000);
  std::uniform_int_distribution<std::uint64_t> slow(1'000'000, 80'000'000);
  LatencyHistogram a;
  LatencyHistogram b;
  LatencyHistogram all;
  for (int i = 0; i < 20'000; ++i) {
    const auto v = fast(rng);
    a.record(v);
    all.record(v);
  }
  for (int i = 0; i < 5'000; ++i) {
    const auto v = slow(rng);
    b.record(v);
    all.record(v);
  }

  a.merge(b);
  EXPECT_EQ(a.count(), all.count());
  EXPECT_EQ(a.min(), all.min());
  EXPECT_EQ(a.max(), all.max());
  EXPECT_DOUBLE_EQ(a.mean(), all.mean());
  for (const double p : kPercentiles) EXPECT_EQ(a.percentile(p), all.percentile(p)) << "p=" << p;
}

TEST(LatencyHistogram, MergeEdgeCases) {
  LatencyHistogram a;
  a.record(10);
  a.record(20);

  // Merging an empty histogram must not disturb min (its internal sentinel) or anything else.
  a.merge(LatencyHistogram{});
  EXPECT_EQ(a.count(), 2U);
  EXPECT_EQ(a.min(), 10U);
  EXPECT_EQ(a.max(), 20U);

  LatencyHistogram empty;
  empty.merge(a);
  EXPECT_EQ(empty.count(), 2U);
  EXPECT_EQ(empty.min(), 10U);
  EXPECT_EQ(empty.max(), 20U);
  EXPECT_DOUBLE_EQ(empty.mean(), 15.0);

  // The 128-bit sum carries across a merge as well.
  LatencyHistogram big1;
  LatencyHistogram big2;
  big1.record(UINT64_MAX);
  big2.record(UINT64_MAX);
  big1.merge(big2);
  EXPECT_NEAR(big1.mean(), 18446744073709551615.0, 18446744073709551615.0 * 1e-12);

  LatencyHistogram other_resolution(7);
  EXPECT_THROW(a.merge(other_resolution), std::invalid_argument);
}

}  // namespace
}  // namespace cell
