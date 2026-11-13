#include "broker/broker_config.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "common/topics.hpp"

namespace cell::broker {
namespace {

constexpr std::int64_t kInt32Max = std::numeric_limits<std::int32_t>::max();

// Integers are read signed so that a negative value in the file is rejected
// instead of silently wrapping to a huge unsigned one.
std::int64_t checked(std::int64_t value, std::string_view key, std::int64_t min, std::int64_t max) {
  if (value < min || value > max) {
    throw ConfigError("config key '" + std::string(key) + "' must be in [" + std::to_string(min) +
                      ", " + std::to_string(max) + "]");
  }
  return value;
}

std::int64_t required(const Json& config, std::string_view key, std::int64_t min,
                      std::int64_t max = kInt32Max) {
  return checked(cfg_get<std::int64_t>(config, key), key, min, max);
}

std::int64_t optional(const Json& config, std::string_view key, std::int64_t fallback,
                      std::int64_t min, std::int64_t max = kInt32Max) {
  return checked(cfg_get_or<std::int64_t>(config, key, fallback), key, min, max);
}

std::string non_empty_string(const Json& node, const char* key, const std::string& where) {
  const auto it = node.find(key);
  if (it == node.end() || !it->is_string() || it->get<std::string>().empty()) {
    throw ConfigError(where + ": '" + key + "' must be a non-empty string");
  }
  return it->get<std::string>();
}

}  // namespace

BrokerConfig BrokerConfig::from_json(const Json& config) {
  using U64 = std::uint64_t;
  BrokerConfig out;
  out.pub_endpoint = cfg_get<std::string>(config, "pub_endpoint");
  out.cmd_endpoint = cfg_get<std::string>(config, "cmd_endpoint");
  out.ring_capacity = static_cast<std::size_t>(required(config, "ring_capacity", 2, 1 << 24));
  out.downsample_hz = cfg_get<double>(config, "downsample_hz");
  if (!(out.downsample_hz > 0.0) || out.downsample_hz > 1e6) {
    throw ConfigError("config key 'downsample_hz' must be in (0, 1e6]");
  }
  out.stats_interval_ms = static_cast<U64>(required(config, "stats_interval_ms", 1));
  out.heartbeat_interval_ms = static_cast<U64>(required(config, "heartbeat_interval_ms", 1));
  out.heartbeat_timeout_ms = static_cast<U64>(required(config, "heartbeat_timeout_ms", 1));
  out.command_timeout_ms = static_cast<U64>(required(config, "command_timeout_ms", 1));
  out.sub_rcvhwm = static_cast<int>(required(config, "sub_rcvhwm", 0));
  out.pub_sndhwm = static_cast<int>(optional(config, "pub_sndhwm", 1000, 0));
  out.zmq_io_threads = static_cast<int>(optional(config, "zmq_io_threads", 2, 1, 64));
  // Capped at 100 ms: the shutdown contract is "no loop blocks longer than that".
  out.poll_timeout_ms = static_cast<U64>(optional(config, "poll_timeout_ms", 50, 1, 100));
  out.io_drain_batch = static_cast<std::size_t>(optional(config, "io_drain_batch", 64, 1));
  out.idle_sleep_us = static_cast<U64>(optional(config, "idle_sleep_us", 500, 1, 100'000));
  out.command_queue_limit =
      static_cast<std::size_t>(optional(config, "command_queue_limit", 64, 1));
  out.supervisor_interval_ms =
      static_cast<U64>(optional(config, "supervisor_interval_ms", 100, 1, 100));
  out.liveness_timeout_ms = static_cast<U64>(optional(config, "liveness_timeout_ms", 2000, 1));

  const Json* streams = cfg_find(config, "streams");
  if (streams == nullptr || !streams->is_array() || streams->empty()) {
    throw ConfigError("config key 'streams' must be a non-empty array");
  }
  std::set<std::string> topics;
  for (std::size_t i = 0; i < streams->size(); ++i) {
    const Json& node = (*streams)[i];
    const std::string where = "config key 'streams[" + std::to_string(i) + "]'";
    if (!node.is_object()) {
      throw ConfigError(where + " must be an object");
    }
    StreamConfig stream;
    stream.endpoint = non_empty_string(node, "endpoint", where);
    stream.process = non_empty_string(node, "process", where);
    if (node.contains("topic") && !node["topic"].is_null()) {
      stream.topic = non_empty_string(node, "topic", where);
      if (stream.topic == topic::kHeartbeat) {
        throw ConfigError(where + ": topic 'hb' is reserved for heartbeats");
      }
      if (!topics.insert(stream.topic).second) {
        throw ConfigError(where + ": duplicate topic '" + stream.topic + "'");
      }
    }
    out.streams.push_back(std::move(stream));
  }

  const Json* routes = cfg_find(config, "routes");
  if (routes == nullptr || !routes->is_object()) {
    throw ConfigError("config key 'routes' must be an object of target -> endpoint");
  }
  for (const auto& [target, endpoint] : routes->items()) {
    if (target.empty() || !endpoint.is_string() || endpoint.get<std::string>().empty()) {
      throw ConfigError("config key 'routes': every entry must map a target to an endpoint");
    }
    out.routes.emplace(target, endpoint.get<std::string>());
  }
  return out;
}

std::vector<std::string> BrokerConfig::expected_processes() const {
  std::vector<std::string> names;
  for (const auto& stream : streams) {
    if (std::find(names.begin(), names.end(), stream.process) == names.end()) {
      names.push_back(stream.process);
    }
  }
  return names;
}

}  // namespace cell::broker
