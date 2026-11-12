#pragma once

#include <cstdint>
#include <random>

#include "common/config.hpp"

namespace cell::plc {

struct BackoffConfig {
  double initial_ms = 100.0;  // first delay, > 0
  double max_ms = 5000.0;     // cap, >= initial_ms
  double multiplier = 2.0;    // growth per failure, >= 1
  double jitter = 0.2;        // +/- fraction applied to each delay, in [0, 1)

  // Reads initial_ms / max_ms / multiplier / jitter from the given JSON object
  // (the "backoff" block) and validates. Throws ConfigError.
  static BackoffConfig from_json(const Json& block);
  void validate() const;
};

// Exponential backoff with jitter. Pure: no clock, and the randomness comes
// from a seeded generator so sequences are reproducible in tests.
//
// The n-th delay after a reset is
//   min(max_ms, initial_ms * multiplier^n) * (1 + U(-jitter, +jitter))
// clamped to [1, max_ms]. Jitter is there so a fleet of clients that lost the
// same server does not reconnect in lockstep.
class Backoff {
 public:
  Backoff(const BackoffConfig& config, std::uint64_t seed);

  // Delay to wait before the next attempt; grows the delay for the one after.
  std::uint64_t next_delay_ms();

  // Call after a success: the next delay starts from initial_ms again.
  void reset() noexcept;

  // Un-jittered delay the next call is based on (for tests and logging).
  double base_ms() const noexcept { return base_ms_; }

 private:
  BackoffConfig config_;
  std::mt19937_64 rng_;
  double base_ms_;
};

// When to try reconnecting: the non-blocking "waiting out the backoff" state
// of the bridge's main loop. Time is injected (ns).
class ReconnectSchedule {
 public:
  explicit ReconnectSchedule(Backoff backoff) : backoff_(std::move(backoff)) {}

  // True when an attempt may be made (immediately after construction or
  // on_connected(), otherwise once the current backoff delay has elapsed).
  bool due(std::uint64_t now_ns) const noexcept { return now_ns >= next_attempt_ns_; }

  // A connect attempt failed, or an established link was lost: schedule the
  // next attempt. Returns the chosen delay in ms.
  std::uint64_t on_failure(std::uint64_t now_ns);

  // The link is up: forget accumulated backoff.
  void on_connected() noexcept;

 private:
  Backoff backoff_;
  std::uint64_t next_attempt_ns_ = 0;
};

}  // namespace cell::plc
