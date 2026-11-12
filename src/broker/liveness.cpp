#include "broker/liveness.hpp"

#include <algorithm>

namespace cell::broker {

LivenessMonitor::LivenessMonitor(std::size_t threads, std::uint64_t start_ns) : threads_(threads) {
  for (auto& thread : threads_) {
    thread.last_change_ns = start_ns;
  }
}

bool LivenessMonitor::update(const std::vector<std::uint64_t>& counters, std::uint64_t now_ns) {
  const std::size_t n = std::min(counters.size(), threads_.size());
  for (std::size_t i = 0; i < n; ++i) {
    if (counters[i] != threads_[i].last_value) {
      threads_[i].last_value = counters[i];
      threads_[i].last_change_ns = now_ns;
      threads_[i].advanced = true;
    }
  }
  const bool all =
      std::all_of(threads_.begin(), threads_.end(), [](const Thread& t) { return t.advanced; });
  if (all) {
    for (auto& thread : threads_) {
      thread.advanced = false;
    }
  }
  return all;
}

std::vector<std::size_t> LivenessMonitor::stalled(std::uint64_t now_ns,
                                                  std::uint64_t timeout_ns) const {
  std::vector<std::size_t> out;
  for (std::size_t i = 0; i < threads_.size(); ++i) {
    if (now_ns > threads_[i].last_change_ns && now_ns - threads_[i].last_change_ns > timeout_ns) {
      out.push_back(i);
    }
  }
  return out;
}

}  // namespace cell::broker
