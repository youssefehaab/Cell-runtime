#pragma once

#include <cstdint>

namespace cell {

// systemd service notification (Type=notify + WatchdogSec). Every call is a
// no-op when not running under systemd or when built without libsystemd, so
// processes use it unconditionally.
class Watchdog {
 public:
  Watchdog();

  // READY=1 — call once initialisation is complete (sockets bound).
  void ready();

  // WATCHDOG=1 — call from the main loop every iteration. Rate-limited
  // internally to half the configured WatchdogSec, so it is cheap to call at
  // kHz rates. Kicking from the main loop (not a helper thread) is the point:
  // a stalled loop stops kicking and systemd restarts the service.
  void kick(std::uint64_t now_mono_ns);

  // STOPPING=1 — call when graceful shutdown begins.
  void stopping();

  bool enabled() const noexcept { return interval_ns_ != 0; }

 private:
  std::uint64_t interval_ns_ = 0;  // 0 = watchdog not requested by systemd
  std::uint64_t last_kick_ns_ = 0;
};

}  // namespace cell
