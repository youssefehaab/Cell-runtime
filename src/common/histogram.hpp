#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cell {

// Log-linear (HdrHistogram-style) histogram for nanosecond latencies.
// Not thread-safe: owned and read by a single thread.
//
// Bucket layout, with b = sub_bucket_bits and S = 2^b:
//   * values in [0, 2S) each get their own bucket (exact);
//   * every further power-of-two range [2^e, 2^(e+1)), e > b, is split into S
//     equal-width linear buckets of width 2^(e-b).
// Equivalently, with shift = max(0, bit_width(v) - 1 - b):
//   index(v) = (shift << b) + (v >> shift)
// which is branch-free apart from the clamp and covers the full uint64 range
// with (65 - b) * S buckets (1920 buckets = 15 KiB for the default b = 5).
//
// Error bound: a bucket of width 2^shift only holds values >= 2^(shift + b), so
// width / value <= 2^-b. percentile() reports the highest value of the bucket
// that contains the requested rank (clamped to the exact max), hence
//   true <= reported < true * (1 + 2^-b)        (b = 5: < 3.125 % high, never low)
// min(), max(), count() and mean() are tracked separately and are exact.
class LatencyHistogram {
 public:
  static constexpr unsigned kDefaultSubBucketBits = 5;
  // Above this the table (> 3 M buckets) stops being cache-friendly and the
  // extra resolution is far below clock jitter anyway.
  static constexpr unsigned kMaxSubBucketBits = 16;

  // Throws std::invalid_argument if sub_bucket_bits > kMaxSubBucketBits.
  explicit LatencyHistogram(unsigned sub_bucket_bits = kDefaultSubBucketBits);

  // O(1), no allocation, any uint64 value.
  void record(std::uint64_t value_ns) noexcept {
    ++counts_[bucket_index(value_ns)];
    ++count_;
    sum_lo_ += value_ns;
    sum_hi_ += static_cast<std::uint64_t>(sum_lo_ < value_ns);  // carry
    if (value_ns < min_) min_ = value_ns;
    if (value_ns > max_) max_ = value_ns;
  }

  std::uint64_t count() const noexcept { return count_; }
  std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }  // exact; 0 when empty
  std::uint64_t max() const noexcept { return max_; }                    // exact; 0 when empty
  double mean() const noexcept;                                          // 0 when empty

  // Value at or below which p percent of the samples fall (nearest-rank:
  // rank = ceil(p/100 * count), at least 1). p is clamped to [0, 100]; NaN is
  // treated as 0. Returns 0 when empty. Never below min() or above max();
  // percentile(0) == min() and percentile(100) == max() exactly.
  // O(buckets between min and max).
  std::uint64_t percentile(double p) const noexcept;

  void reset() noexcept;

  // Adds other's samples. Throws std::invalid_argument on differing sub_bucket_bits.
  void merge(const LatencyHistogram& other);

  unsigned sub_bucket_bits() const noexcept { return sub_bucket_bits_; }
  std::size_t bucket_count() const noexcept { return counts_.size(); }

 private:
  std::size_t bucket_index(std::uint64_t v) const noexcept {
    const unsigned width = static_cast<unsigned>(std::bit_width(v));
    const unsigned shift = width > sub_bucket_bits_ + 1 ? width - 1 - sub_bucket_bits_ : 0;
    return (static_cast<std::size_t>(shift) << sub_bucket_bits_) +
           static_cast<std::size_t>(v >> shift);
  }

  // Largest value that maps to bucket `index` (inverse of bucket_index).
  std::uint64_t bucket_upper_bound(std::size_t index) const noexcept;

  unsigned sub_bucket_bits_;
  std::vector<std::uint64_t> counts_;
  std::uint64_t count_ = 0;
  std::uint64_t min_ = UINT64_MAX;
  std::uint64_t max_ = 0;
  // 128-bit running sum (hi:lo) so the mean stays exact even if samples near
  // UINT64_MAX are recorded; unsigned __int128 is not ISO C++ (-Wpedantic).
  std::uint64_t sum_lo_ = 0;
  std::uint64_t sum_hi_ = 0;
};

}  // namespace cell
