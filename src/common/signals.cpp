#include "common/signals.hpp"

#include <atomic>
#include <csignal>

namespace cell {
namespace {

std::atomic<bool> g_shutdown{false};
static_assert(std::atomic<bool>::is_always_lock_free, "flag must be async-signal-safe");

extern "C" void on_shutdown_signal(int /*signo*/) {
  g_shutdown.store(true, std::memory_order_relaxed);
}

}  // namespace

void install_shutdown_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_shutdown_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // deliberately no SA_RESTART
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  // A consumer vanishing mid-write must not kill the process.
  struct sigaction ign {};
  ign.sa_handler = SIG_IGN;
  sigemptyset(&ign.sa_mask);
  sigaction(SIGPIPE, &ign, nullptr);
}

bool shutdown_requested() noexcept { return g_shutdown.load(std::memory_order_relaxed); }

void request_shutdown() noexcept { g_shutdown.store(true, std::memory_order_relaxed); }

void reset_shutdown_flag() noexcept { g_shutdown.store(false, std::memory_order_relaxed); }

}  // namespace cell
