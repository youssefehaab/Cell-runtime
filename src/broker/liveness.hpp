#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cell::broker {

// Decides when the systemd watchdog may be kicked. Each worker thread bumps
// its own counter once per loop iteration; the supervisor samples all of them.
// A kick is allowed only once EVERY counter has moved since the previous kick,
// so one stalled thread (deadlock, livelock inside a callee, blocked syscall)
// silences the watchdog even while the others keep running. No clock inside:
// time is passed in.
class LivenessMonitor {
 public:
  LivenessMonitor(std::size_t threads, std::uint64_t start_ns);

  // Feed the current counter values. Returns true when every thread has made
  // progress since the last time this returned true.
  bool update(const std::vector<std::uint64_t>& counters, std::uint64_t now_ns);

  // Indices of threads whose counter has not moved for longer than `timeout_ns`.
  std::vector<std::size_t> stalled(std::uint64_t now_ns, std::uint64_t timeout_ns) const;

 private:
  struct Thread {
    std::uint64_t last_value = 0;
    std::uint64_t last_change_ns = 0;
    bool advanced = false;
  };
  std::vector<Thread> threads_;
};

}  // namespace cell::broker
