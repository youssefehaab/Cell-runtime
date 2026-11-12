#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/config.hpp"

namespace cell::broker {

// One device telemetry socket. Every socket also carries the "hb" topic of the
// process that owns it; an entry with an empty topic is heartbeat-only.
struct StreamConfig {
  std::string topic;
  std::string endpoint;
  std::string process;

  bool has_data() const noexcept { return !topic.empty(); }
};

// Every broker tunable, validated once at start-up.
struct BrokerConfig {
  std::string pub_endpoint;  // XPUB for consumers
  std::string cmd_endpoint;  // ROUTER for external commands
  std::size_t ring_capacity = 0;
  double downsample_hz = 0.0;
  std::uint64_t stats_interval_ms = 0;
  std::uint64_t heartbeat_interval_ms = 0;
  std::uint64_t heartbeat_timeout_ms = 0;
  std::uint64_t command_timeout_ms = 0;
  int sub_rcvhwm = 0;
  int pub_sndhwm = 0;
  int zmq_io_threads = 0;
  // Upper bound on how long the I/O and command threads sleep in zmq::poll, and
  // therefore on how quickly they notice a shutdown request.
  std::uint64_t poll_timeout_ms = 0;
  // Messages taken from one readable socket before the others get a turn.
  std::size_t io_drain_batch = 0;
  // Processing-thread sleep when the ring is empty.
  std::uint64_t idle_sleep_us = 0;
  // In-flight + queued commands allowed per target before the broker answers "busy".
  std::size_t command_queue_limit = 0;
  // How often the main thread compares the worker liveness counters.
  std::uint64_t supervisor_interval_ms = 0;
  // A worker that makes no progress for this long is reported as stalled.
  std::uint64_t liveness_timeout_ms = 0;
  std::vector<StreamConfig> streams;
  std::map<std::string, std::string> routes;  // Command.target -> device REP endpoint

  // Throws ConfigError naming the offending key.
  static BrokerConfig from_json(const Json& config);

  // Distinct process names from `streams`, in first-appearance order.
  std::vector<std::string> expected_processes() const;
};

}  // namespace cell::broker
