#pragma once

#include <cstdint>
#include <ctime>

namespace cell {

// CLOCK_MONOTONIC in nanoseconds. All processes on one host (including
// containers sharing the host kernel) read the same clock, which is what makes
// send-timestamp latency measurement valid without clock sync.
inline std::uint64_t mono_now_ns() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(ts.tv_nsec);
}

// Sleep until an absolute CLOCK_MONOTONIC deadline. Returns early (false) if
// interrupted by a signal, so periodic loops can re-check for shutdown.
inline bool sleep_until_ns(std::uint64_t deadline_ns) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(deadline_ns / 1'000'000'000ULL);
  ts.tv_nsec = static_cast<long>(deadline_ns % 1'000'000'000ULL);
  return clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == 0;
}

}  // namespace cell
