#pragma once

#include <cstdint>

namespace cell::sensor {

// Fault-injection state for "the sensor dropped off". Pure: time is passed in
// (CLOCK_MONOTONIC ns in production, anything in tests).
class DisconnectFault {
 public:
  // Start (or restart) a disconnect at `now_ns`. duration_ms == 0 means "until
  // reconnect()"; otherwise the fault expires on its own after duration_ms.
  void disconnect(std::uint64_t now_ns, std::uint32_t duration_ms) noexcept;

  // End the fault early. No-op if none is active.
  void reconnect() noexcept;

  // True while the sensor should stay silent. Clears an expired timed fault.
  bool active(std::uint64_t now_ns) noexcept;

 private:
  bool active_ = false;
  bool timed_ = false;
  std::uint64_t until_ns_ = 0;
};

}  // namespace cell::sensor
