#include "common/watchdog.hpp"

#if CELL_HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

namespace cell {

#if CELL_HAVE_SYSTEMD

Watchdog::Watchdog() {
  std::uint64_t usec = 0;
  if (sd_watchdog_enabled(0, &usec) > 0 && usec > 0) {
    interval_ns_ = usec * 1000ULL / 2ULL;
  }
}

void Watchdog::ready() { sd_notify(0, "READY=1"); }

void Watchdog::kick(std::uint64_t now_mono_ns) {
  if (interval_ns_ == 0 || now_mono_ns - last_kick_ns_ < interval_ns_) {
    return;
  }
  last_kick_ns_ = now_mono_ns;
  sd_notify(0, "WATCHDOG=1");
}

void Watchdog::stopping() { sd_notify(0, "STOPPING=1"); }

#else

Watchdog::Watchdog() = default;
void Watchdog::ready() {}
void Watchdog::kick(std::uint64_t /*now_mono_ns*/) {}
void Watchdog::stopping() {}

#endif

}  // namespace cell
