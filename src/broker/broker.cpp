#include "broker/broker.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <exception>
#include <thread>
#include <vector>
#include <zmq.hpp>

#include "broker/liveness.hpp"
#include "broker/shared.hpp"
#include "common/clock.hpp"
#include "common/signals.hpp"
#include "common/watchdog.hpp"

namespace cell::broker {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;

struct Worker {
  const char* name;
  ThreadState Shared::*state;
  void (*body)(Shared&);
};

constexpr std::array<Worker, 3> kWorkers{{
    {"io", &Shared::io, run_io_thread},
    {"processing", &Shared::processing, run_processing_thread},
    {"command", &Shared::command, run_command_thread},
}};

// A worker that dies takes the whole process down (exit code 1) so the service
// manager restarts it; a broker with one thread missing would look alive while
// doing only part of its job.
void run_worker(const Worker& worker, Shared& shared) {
  try {
    worker.body(shared);
  } catch (const std::exception& e) {
    spdlog::critical("{} thread failed: {}", worker.name, e.what());
    shared.failed.store(true);
    request_shutdown();
  }
  (shared.*worker.state).done.store(true);
}

bool all_ready(const Shared& shared) {
  for (const Worker& worker : kWorkers) {
    if (!(shared.*worker.state).ready.load()) {
      return false;
    }
  }
  return true;
}

}  // namespace

int run_broker(const BrokerConfig& config) {
  // Declaration order is destruction order in reverse: threads are joined
  // first (closing their sockets), then the ring releases any messages still
  // in it, and only then is the context terminated.
  zmq::context_t ctx(config.zmq_io_threads);
  Shared shared(config, ctx);
  Watchdog watchdog;
  const std::uint64_t interval_ns = config.supervisor_interval_ms * kNsPerMs;
  {
    std::vector<std::jthread> threads;
    threads.reserve(kWorkers.size());
    for (const Worker& worker : kWorkers) {
      threads.emplace_back([&worker, &shared] { run_worker(worker, shared); });
    }

    // READY=1 only once every socket is bound/connected. A start-up failure
    // (endpoint in use, ...) requests shutdown, which also ends this wait.
    while (!shutdown_requested() && !all_ready(shared)) {
      sleep_until_ns(mono_now_ns() + kNsPerMs);
    }
    if (!shutdown_requested()) {
      watchdog.ready();
      spdlog::info("broker running: {} streams, {} command routes, ring capacity {}",
                   config.streams.size(), config.routes.size(), shared.ring.capacity());
    }

    // The main thread does no work of its own, so it cannot vouch for the
    // others by merely running. It kicks the watchdog only after seeing every
    // worker's loop counter move; if any one thread stops making progress the
    // kicks stop and systemd restarts the service.
    LivenessMonitor liveness(kWorkers.size(), mono_now_ns());
    std::vector<std::uint64_t> beats(kWorkers.size());
    bool stall_reported = false;
    while (!shutdown_requested()) {
      const std::uint64_t now = mono_now_ns();
      for (std::size_t i = 0; i < kWorkers.size(); ++i) {
        beats[i] = (shared.*kWorkers[i].state).beats.load(std::memory_order_relaxed);
      }
      if (liveness.update(beats, now)) {
        watchdog.kick(now);
      }
      const auto stalled = liveness.stalled(now, config.liveness_timeout_ms * kNsPerMs);
      if (!stalled.empty() && !stall_reported) {
        for (const std::size_t i : stalled) {
          spdlog::critical("{} thread has made no progress for {} ms; withholding watchdog kicks",
                           kWorkers[i].name, config.liveness_timeout_ms);
        }
      }
      stall_reported = !stalled.empty();
      sleep_until_ns(now + interval_ns);
    }
    watchdog.stopping();
  }  // jthreads join here

  const bool failed = shared.failed.load();
  spdlog::log(failed ? spdlog::level::err : spdlog::level::info, "broker stopped{}",
              failed ? " after a thread failure" : " cleanly");
  return failed ? 1 : 0;
}

}  // namespace cell::broker
