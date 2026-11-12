#include "plc_bridge/backoff.hpp"

#include <algorithm>
#include <cmath>

namespace cell::plc {

BackoffConfig BackoffConfig::from_json(const Json& block) {
  BackoffConfig c;
  c.initial_ms = cfg_get<double>(block, "initial_ms");
  c.max_ms = cfg_get<double>(block, "max_ms");
  c.multiplier = cfg_get<double>(block, "multiplier");
  c.jitter = cfg_get<double>(block, "jitter");
  c.validate();
  return c;
}

void BackoffConfig::validate() const {
  if (!std::isfinite(initial_ms) || initial_ms < 1.0) {
    throw ConfigError("backoff.initial_ms must be at least 1");
  }
  if (!std::isfinite(max_ms) || max_ms < initial_ms) {
    throw ConfigError("backoff.max_ms must be >= backoff.initial_ms");
  }
  if (!std::isfinite(multiplier) || multiplier < 1.0) {
    throw ConfigError("backoff.multiplier must be >= 1");
  }
  if (!std::isfinite(jitter) || jitter < 0.0 || jitter >= 1.0) {
    throw ConfigError("backoff.jitter must be in [0, 1)");
  }
}

Backoff::Backoff(const BackoffConfig& config, std::uint64_t seed)
    : config_(config), rng_(seed), base_ms_(config.initial_ms) {
  config_.validate();
}

std::uint64_t Backoff::next_delay_ms() {
  // 53 random bits -> uniform double in [0, 1). Done by hand rather than with
  // std::uniform_real_distribution so a seed gives the same sequence on every
  // standard library.
  const double unit = static_cast<double>(rng_() >> 11) * 0x1.0p-53;
  const double factor = 1.0 + config_.jitter * (2.0 * unit - 1.0);
  const double delay = std::clamp(base_ms_ * factor, 1.0, config_.max_ms);
  base_ms_ = std::min(base_ms_ * config_.multiplier, config_.max_ms);
  return static_cast<std::uint64_t>(std::llround(delay));
}

void Backoff::reset() noexcept { base_ms_ = config_.initial_ms; }

std::uint64_t ReconnectSchedule::on_failure(std::uint64_t now_ns) {
  const std::uint64_t delay_ms = backoff_.next_delay_ms();
  next_attempt_ns_ = now_ns + delay_ms * 1'000'000ULL;
  return delay_ms;
}

void ReconnectSchedule::on_connected() noexcept {
  backoff_.reset();
  next_attempt_ns_ = 0;
}

}  // namespace cell::plc
