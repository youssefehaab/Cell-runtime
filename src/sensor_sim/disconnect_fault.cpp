#include "sensor_sim/disconnect_fault.hpp"

namespace cell::sensor {

void DisconnectFault::disconnect(std::uint64_t now_ns, std::uint32_t duration_ms) noexcept {
  active_ = true;
  timed_ = duration_ms != 0;
  until_ns_ = now_ns + static_cast<std::uint64_t>(duration_ms) * 1'000'000ULL;
}

void DisconnectFault::reconnect() noexcept { active_ = false; }

bool DisconnectFault::active(std::uint64_t now_ns) noexcept {
  if (active_ && timed_ && now_ns >= until_ns_) {
    active_ = false;
  }
  return active_;
}

}  // namespace cell::sensor
