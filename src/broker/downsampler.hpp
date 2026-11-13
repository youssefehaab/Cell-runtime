#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace cell::broker {

// Fires once per period on a fixed grid (deadline += period), so jitter in one
// tick does not accumulate as drift. If the caller falls a whole period or more
// behind, the missed ticks are skipped rather than fired in a burst.
class PeriodicTimer {
 public:
  PeriodicTimer(std::uint64_t period_ns, std::uint64_t start_ns) noexcept
      : period_ns_(period_ns == 0 ? 1 : period_ns), next_ns_(start_ns + period_ns_) {}

  bool due(std::uint64_t now_ns) noexcept {
    if (now_ns < next_ns_) {
      return false;
    }
    next_ns_ += period_ns_;
    if (next_ns_ <= now_ns) {
      next_ns_ = now_ns + period_ns_;
    }
    return true;
  }

  std::uint64_t next_ns() const noexcept { return next_ns_; }
  std::uint64_t period_ns() const noexcept { return period_ns_; }

 private:
  std::uint64_t period_ns_;
  std::uint64_t next_ns_;
};

// Latest-value downsampler: keeps only the newest pushed value and hands it out
// at most once per period, and only if something new arrived since the last
// emission. Intermediate values are discarded, never queued, so consumers
// always get the freshest sample and no backlog can build up. The first value
// is emitted immediately; after that emissions sit on a fixed grid, so a 1 kHz
// input yields exactly the configured rate rather than "period + one input
// interval".
template <typename T>
class Downsampler {
 public:
  explicit Downsampler(std::uint64_t period_ns) noexcept
      : period_ns_(period_ns == 0 ? 1 : period_ns) {}

  void push(T value) {
    latest_.emplace(std::move(value));
    ++received_;
  }

  // Moves the pending value out if one is due.
  std::optional<T> poll(std::uint64_t now_ns) {
    if (!latest_.has_value() || now_ns < next_ns_) {
      return std::nullopt;
    }
    next_ns_ += period_ns_;
    if (next_ns_ <= now_ns) {
      next_ns_ = now_ns + period_ns_;  // first value, or the input paused: restart the grid
    }
    std::optional<T> out(std::move(*latest_));
    latest_.reset();
    ++emitted_;
    return out;
  }

  bool pending() const noexcept { return latest_.has_value(); }
  std::uint64_t received() const noexcept { return received_; }
  std::uint64_t emitted() const noexcept { return emitted_; }

 private:
  std::uint64_t period_ns_;
  std::uint64_t next_ns_ = 0;
  std::optional<T> latest_;
  std::uint64_t received_ = 0;
  std::uint64_t emitted_ = 0;
};

}  // namespace cell::broker
