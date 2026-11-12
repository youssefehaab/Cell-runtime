// arm_sim: simulated multi-axis arm. Publishes JointState at a fixed rate and
// serves MoveTo / Stop / EStop / Reset on a REP socket.
#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <vector>
#include <zmq.hpp>

#include "arm_sim/arm_model.hpp"
#include "cell.pb.h"
#include "common/app.hpp"
#include "common/clock.hpp"
#include "common/config.hpp"
#include "common/heartbeat.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/watchdog.hpp"
#include "common/zmq_util.hpp"

namespace {

using cell::arm::ArmConfig;
using cell::arm::ArmModel;
using cell::arm::CommandResult;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;

// Answer every command waiting on the REP socket without blocking. Every
// request gets a reply - a REP socket that skips one is stuck forever.
void drain_commands(zmq::socket_t& rep, ArmModel& arm, std::uint64_t& reply_seq) {
  std::vector<zmq::message_t> frames;
  while (cell::recv_multipart(rep, frames, /*dontwait=*/true)) {
    cell::Command cmd;
    CommandResult result;
    if (frames.size() != 1 || !cell::parse_proto(frames.front(), cmd)) {
      result = CommandResult::failure("malformed command: expected one Command frame");
    } else {
      const cell::ArmMode before = arm.mode();
      result = arm.handle(cmd);
      if (!result.ok) {
        spdlog::warn("command rejected: {}", result.error);
      } else if (arm.mode() != before) {
        spdlog::info("mode {} -> {}", cell::ArmMode_Name(before), cell::ArmMode_Name(arm.mode()));
      }
    }
    cell::CommandReply reply;
    reply.set_ok(result.ok);
    reply.set_error(result.error);
    cell::fill_header(*reply.mutable_h(), reply_seq++, cell::process::kArmSim);
    cell::send_proto(rep, reply);
  }
}

int run(const cell::Json& config) {
  const auto telemetry_endpoint = cell::cfg_get<std::string>(config, "telemetry_endpoint");
  const auto cmd_endpoint = cell::cfg_get<std::string>(config, "cmd_endpoint");
  const auto rate_hz = cell::cfg_get<double>(config, "rate_hz");
  const auto heartbeat_ms = cell::cfg_get<std::uint64_t>(config, "heartbeat_interval_ms");
  if (!(rate_hz > 0.0) || rate_hz > 1e6) {
    throw cell::ConfigError("config key 'rate_hz' must be in (0, 1e6]");
  }
  ArmModel arm(ArmConfig::from_json(config));

  zmq::context_t ctx(1);
  zmq::socket_t pub = cell::make_pub(ctx, telemetry_endpoint);
  zmq::socket_t rep = cell::make_rep(ctx, cmd_endpoint);

  cell::HeartbeatEmitter heartbeat(cell::process::kArmSim, heartbeat_ms);
  heartbeat.set_status(cell::PROCESS_OK);
  cell::Watchdog watchdog;
  watchdog.ready();
  spdlog::info("arm_sim running: {} joints at {} Hz, telemetry {}, commands {}", arm.joints(),
               rate_hz, telemetry_endpoint, cmd_endpoint);

  const auto period_ns = static_cast<std::uint64_t>(static_cast<double>(kNsPerSec) / rate_hz);
  const double dt = 1.0 / rate_hz;
  cell::JointState state;
  std::uint64_t seq = 0;
  std::uint64_t reply_seq = 0;
  std::uint64_t overruns = 0;
  std::uint64_t deadline = cell::mono_now_ns() + period_ns;

  while (!cell::shutdown_requested()) {
    drain_commands(rep, arm, reply_seq);
    const cell::ArmMode last_mode = arm.mode();

    arm.step(dt);
    if (arm.mode() != last_mode) {
      spdlog::info("mode {} -> {}", cell::ArmMode_Name(last_mode), cell::ArmMode_Name(arm.mode()));
    }

    arm.fill(state);
    cell::fill_header(*state.mutable_h(), seq, cell::process::kArmSim);
    if (cell::send_proto(pub, cell::topic::kArmJoint, state)) {
      ++seq;
    }

    const std::uint64_t now = cell::mono_now_ns();
    heartbeat.tick(pub, now);
    watchdog.kick(now);

    // Absolute deadlines: jitter in one tick does not accumulate as drift. If
    // we fell more than a tick behind (descheduled, debugger, ...), drop the
    // missed ticks and resynchronise instead of bursting to catch up.
    deadline += period_ns;
    if (now > deadline + period_ns) {
      if (++overruns == 1) {
        spdlog::warn("control loop overran its {} ns period; resynchronising", period_ns);
      }
      deadline = now + period_ns;
    }
    cell::sleep_until_ns(deadline);
  }

  watchdog.stopping();
  heartbeat.set_status(cell::PROCESS_STOPPING);
  heartbeat.publish_now(pub, cell::mono_now_ns());
  spdlog::info("arm_sim stopped cleanly: {} joint states published, {} loop overruns", seq,
               overruns);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return cell::run_app(argc, argv, {cell::process::kArmSim, "Simulated multi-axis robot arm"}, run);
}
