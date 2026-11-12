// sensor_sim: simulated depth sensor. Publishes PointCloudFrame at a fixed
// frame rate and can simulate dropping off (FaultInject on the REP socket).
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include <zmq.hpp>

#include "cell.pb.h"
#include "common/app.hpp"
#include "common/clock.hpp"
#include "common/config.hpp"
#include "common/heartbeat.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/watchdog.hpp"
#include "common/zmq_util.hpp"
#include "sensor_sim/disconnect_fault.hpp"
#include "sensor_sim/frame_generator.hpp"

namespace {

using cell::sensor::DisconnectFault;
using cell::sensor::FrameGenerator;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
// Longest single wait: bounds shutdown latency and heartbeat lateness at low fps.
constexpr std::uint64_t kMaxWaitMs = 50;
constexpr const char* kDisconnectedDetail = "sensor disconnected (fault injection)";

// Running min/avg/max of a duration, reported in milliseconds.
struct DurationStats {
  std::uint64_t count = 0;
  std::uint64_t sum_ns = 0;
  std::uint64_t max_ns = 0;

  void add(std::uint64_t ns) noexcept {
    ++count;
    sum_ns += ns;
    max_ns = std::max(max_ns, ns);
  }
  double avg_ms() const noexcept {
    return count == 0 ? 0.0 : static_cast<double>(sum_ns) / static_cast<double>(count) / 1e6;
  }
  double max_ms() const noexcept { return static_cast<double>(max_ns) / 1e6; }
};

struct CommandOutcome {
  bool ok = true;
  std::string error;
};

CommandOutcome apply_command(const cell::Command& cmd, DisconnectFault& fault,
                             std::uint64_t now_ns) {
  if (cmd.action_case() != cell::Command::kFaultInject) {
    return {false, "unsupported action for sensor: only fault_inject is implemented"};
  }
  const cell::FaultInject& inject = cmd.fault_inject();
  switch (inject.kind()) {
    case cell::FaultInject::DISCONNECT:
      fault.disconnect(now_ns, inject.duration_ms());
      return {};
    case cell::FaultInject::RECONNECT:
      fault.reconnect();
      return {};
    default:
      return {false, "fault_inject kind must be DISCONNECT or RECONNECT"};
  }
}

// Answer every waiting command without blocking; every request gets a reply.
void drain_commands(zmq::socket_t& rep, DisconnectFault& fault, std::uint64_t& reply_seq) {
  std::vector<zmq::message_t> frames;
  while (cell::recv_multipart(rep, frames, /*dontwait=*/true)) {
    cell::Command cmd;
    CommandOutcome outcome;
    if (frames.size() != 1 || !cell::parse_proto(frames.front(), cmd)) {
      outcome = {false, "malformed command: expected one Command frame"};
    } else {
      outcome = apply_command(cmd, fault, cell::mono_now_ns());
    }
    if (!outcome.ok) {
      spdlog::warn("command rejected: {}", outcome.error);
    }
    cell::CommandReply reply;
    reply.set_ok(outcome.ok);
    reply.set_error(outcome.error);
    cell::fill_header(*reply.mutable_h(), reply_seq++, cell::process::kSensorSim);
    cell::send_proto(rep, reply);
  }
}

int run(const cell::Json& config) {
  const auto frames_endpoint = cell::cfg_get<std::string>(config, "frames_endpoint");
  const auto cmd_endpoint = cell::cfg_get<std::string>(config, "cmd_endpoint");
  const auto width = cell::cfg_get<std::uint32_t>(config, "width");
  const auto height = cell::cfg_get<std::uint32_t>(config, "height");
  const auto fps = cell::cfg_get<double>(config, "fps");
  const auto heartbeat_ms = cell::cfg_get<std::uint64_t>(config, "heartbeat_interval_ms");
  const auto sndhwm = cell::cfg_get<int>(config, "sndhwm");
  const auto stats_interval_s = cell::cfg_get_or<std::uint64_t>(config, "stats_interval_s", 10);
  if (width == 0 || height == 0 || static_cast<std::uint64_t>(width) * height > 16'000'000ULL) {
    throw cell::ConfigError("config keys 'width' x 'height' must be between 1 and 16M points");
  }
  if (!(fps > 0.0) || fps > 10'000.0) {
    throw cell::ConfigError("config key 'fps' must be in (0, 10000]");
  }
  if (sndhwm < 1) {
    throw cell::ConfigError("config key 'sndhwm' must be at least 1");
  }

  FrameGenerator generator(width, height, fps);

  zmq::context_t ctx(1);
  // A small send high-water mark: frames are megabytes each, so a stalled
  // subscriber must cost a bounded amount of memory (it loses frames instead).
  zmq::socket_t pub = cell::make_pub(ctx, frames_endpoint, cell::SocketOptions{.sndhwm = sndhwm});
  zmq::socket_t rep = cell::make_rep(ctx, cmd_endpoint);

  cell::HeartbeatEmitter heartbeat(cell::process::kSensorSim, heartbeat_ms);
  heartbeat.set_status(cell::PROCESS_OK);
  cell::Watchdog watchdog;
  watchdog.ready();
  spdlog::info(
      "sensor_sim running: {}x{} points ({} bytes/frame) at {} fps, frames {}, commands {}", width,
      height, generator.frame_bytes(), fps, frames_endpoint, cmd_endpoint);

  // The frame message is reused: `points` is sized once and regenerated in
  // place, so the steady state allocates nothing for generation.
  cell::PointCloudFrame frame;
  frame.set_width(width);
  frame.set_height(height);
  frame.mutable_points()->resize(generator.frame_bytes());

  const auto period_ns = static_cast<std::uint64_t>(static_cast<double>(kNsPerSec) / fps);
  const std::uint64_t stats_interval_ns = stats_interval_s * kNsPerSec;
  DisconnectFault fault;
  DurationStats generate_stats;
  DurationStats publish_stats;
  DurationStats window_generate;
  DurationStats window_publish;
  std::uint64_t seq = 0;  // doubles as the frame index: no gap across a disconnect
  std::uint64_t reply_seq = 0;
  std::uint64_t overruns = 0;
  bool disconnected = false;
  std::uint64_t deadline = cell::mono_now_ns() + period_ns;
  std::uint64_t next_stats = cell::mono_now_ns() + stats_interval_ns;

  while (!cell::shutdown_requested()) {
    // Wait for the frame deadline, staying responsive to commands, heartbeats
    // and shutdown rather than sleeping blind.
    std::uint64_t now = cell::mono_now_ns();
    while (!cell::shutdown_requested()) {
      now = cell::mono_now_ns();
      const bool now_disconnected = fault.active(now);
      if (now_disconnected != disconnected) {
        disconnected = now_disconnected;
        if (disconnected) {
          spdlog::warn("sensor disconnected (fault injection): frames paused");
          heartbeat.set_status(cell::PROCESS_DEGRADED, kDisconnectedDetail);
        } else {
          spdlog::info("sensor reconnected: frames resume at seq {}", seq);
          heartbeat.set_status(cell::PROCESS_OK);
        }
        heartbeat.publish_now(pub, now);  // make the transition visible immediately
      }
      heartbeat.tick(pub, now);
      watchdog.kick(now);
      if (now >= deadline) {
        break;
      }
      const std::uint64_t wait_ms = (deadline - now) / kNsPerMs;
      if (wait_ms >= 1) {
        if (cell::poll_in(rep, std::chrono::milliseconds(std::min(wait_ms, kMaxWaitMs)))) {
          drain_commands(rep, fault, reply_seq);
        }
      } else {
        cell::sleep_until_ns(deadline);  // sub-millisecond remainder
      }
    }
    if (cell::shutdown_requested()) {
      break;
    }
    // Covers the case where frames are back-to-back and the wait above never
    // polled the command socket. A fault change takes effect at the next wait.
    drain_commands(rep, fault, reply_seq);

    if (!disconnected) {
      const std::uint64_t t0 = cell::mono_now_ns();
      std::string& points = *frame.mutable_points();
      generator.generate(seq, std::as_writable_bytes(std::span<char>(points)));
      const std::uint64_t t1 = cell::mono_now_ns();
      cell::fill_header(*frame.mutable_h(), seq, cell::process::kSensorSim);
      if (cell::send_proto(pub, cell::topic::kSensorFrame, frame)) {
        ++seq;
      }
      const std::uint64_t t2 = cell::mono_now_ns();
      generate_stats.add(t1 - t0);
      publish_stats.add(t2 - t1);
      window_generate.add(t1 - t0);
      window_publish.add(t2 - t1);
    }

    now = cell::mono_now_ns();
    if (stats_interval_ns != 0 && now >= next_stats) {
      spdlog::info("frames={} generate avg/max {:.3f}/{:.3f} ms, publish avg/max {:.3f}/{:.3f} ms",
                   seq, window_generate.avg_ms(), window_generate.max_ms(), window_publish.avg_ms(),
                   window_publish.max_ms());
      window_generate = {};
      window_publish = {};
      next_stats = now + stats_interval_ns;
    }

    // Absolute deadlines (no drift); after a long stall, resynchronise rather
    // than emit a burst of late frames.
    deadline += period_ns;
    if (now > deadline + period_ns) {
      if (++overruns == 1) {
        spdlog::warn("frame loop overran its {} ns period; resynchronising", period_ns);
      }
      deadline = now + period_ns;
    }
  }

  watchdog.stopping();
  heartbeat.set_status(cell::PROCESS_STOPPING);
  heartbeat.publish_now(pub, cell::mono_now_ns());
  spdlog::info(
      "sensor_sim stopped cleanly: {} frames, generate avg/max {:.3f}/{:.3f} ms, publish avg/max "
      "{:.3f}/{:.3f} ms, {} overruns",
      seq, generate_stats.avg_ms(), generate_stats.max_ms(), publish_stats.avg_ms(),
      publish_stats.max_ms(), overruns);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return cell::run_app(argc, argv, {cell::process::kSensorSim, "Simulated depth sensor"}, run);
}
