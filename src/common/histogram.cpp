#include "common/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cell {
namespace {

constexpr unsigned kValueBits = std::numeric_limits<std::uint64_t>::digits;
constexpr double kPercentMax = 100.0;
constexpr double kTwoPow64 = 18446744073709551616.0;

std::size_t bucket_count_for(unsigned sub_bucket_bits) {
  if (sub_bucket_bits > LatencyHistogram::kMaxSubBucketBits) {
    throw std::invalid_argument("LatencyHistogram: sub_bucket_bits out of range");
  }
  // shift ranges over [0, 63 - b]; each shift owns S indices, and shift 0 owns 2S.
  return static_cast<std::size_t>(kValueBits + 1 - sub_bucket_bits) << sub_bucket_bits;
}

}  // namespace

LatencyHistogram::LatencyHistogram(unsigned sub_bucket_bits)
    : sub_bucket_bits_(sub_bucket_bits), counts_(bucket_count_for(sub_bucket_bits), 0) {}

double LatencyHistogram::mean() const noexcept {
  if (count_ == 0) return 0.0;
  const double sum = static_cast<double>(sum_hi_) * kTwoPow64 + static_cast<double>(sum_lo_);
  return sum / static_cast<double>(count_);
}

std::uint64_t LatencyHistogram::bucket_upper_bound(std::size_t index) const noexcept {
  const std::size_t range = index >> sub_bucket_bits_;
  if (range == 0) return index;  // first S buckets: one value each
  const unsigned shift = static_cast<unsigned>(range - 1);
  const std::uint64_t sub_buckets = std::uint64_t{1} << sub_bucket_bits_;
  const std::uint64_t mantissa = sub_buckets + (index & (sub_buckets - 1));
  const std::uint64_t lower = mantissa << shift;
  return lower + ((std::uint64_t{1} << shift) - 1);
}

std::uint64_t LatencyHistogram::percentile(double p) const noexcept {
  if (count_ == 0) return 0;
  const double clamped = p > 0.0 ? std::min(p, kPercentMax) : 0.0;  // also maps NaN to 0
  const double exact_rank = std::ceil(clamped / kPercentMax * static_cast<double>(count_));
  // exact_rank <= count_ mathematically, but count_ -> double may round up.
  const std::uint64_t rank =
      exact_rank >= static_cast<double>(count_)
          ? count_
          : std::max<std::uint64_t>(static_cast<std::uint64_t>(exact_rank), 1);
  // The extremes are tracked exactly; no need to pay bucket error for them.
  if (rank == 1) return min_;
  if (rank == count_) return max_;

  // Only buckets between min and max can be populated.
  const std::size_t last = bucket_index(max_);
  std::uint64_t seen = 0;
  for (std::size_t i = bucket_index(min_); i <= last; ++i) {
    seen += counts_[i];
    if (seen >= rank) return std::min(bucket_upper_bound(i), max_);
  }
  return max_;  // unreachable: the counts sum to count_ >= rank
}

void LatencyHistogram::reset() noexcept {
  std::fill(counts_.begin(), counts_.end(), std::uint64_t{0});
  count_ = 0;
  min_ = UINT64_MAX;
  max_ = 0;
  sum_lo_ = 0;
  sum_hi_ = 0;
}

void LatencyHistogram::merge(const LatencyHistogram& other) {
  if (other.sub_bucket_bits_ != sub_bucket_bits_) {
    throw std::invalid_argument("LatencyHistogram::merge: sub_bucket_bits mismatch");
  }
  for (std::size_t i = 0; i < counts_.size(); ++i) {
    counts_[i] += other.counts_[i];
  }
  count_ += other.count_;
  min_ = std::min(min_, other.min_);
  max_ = std::max(max_, other.max_);
  sum_lo_ += other.sum_lo_;
  sum_hi_ += other.sum_hi_ + static_cast<std::uint64_t>(sum_lo_ < other.sum_lo_);
}

}  // namespace cell
