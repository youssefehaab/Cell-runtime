// plc_bridge: Modbus-TCP client for the conveyor PLC. Polls the PLC, publishes
// ConveyorState, forwards commands, and rides out PLC outages by reconnecting
// with exponential backoff.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <utility>
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
#include "plc_bridge/backoff.hpp"
#include "plc_bridge/conveyor_map.hpp"
#include "plc_bridge/modbus_link.hpp"

namespace {

using cell::plc::Backoff;
using cell::plc::BackoffConfig;
using cell::plc::ModbusLink;
using cell::plc::ReconnectSchedule;
using cell::plc::RegisterMap;

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;
// Input registers this far apart or closer are fetched with one request.
constexpr int kMaxRegisterSpan = 16;

struct CommandOutcome {
  bool ok = true;
  std::string error;
};

// Owns the Modbus link and its up/down state machine:
//
//   DOWN --connect ok--> UP --read/write error--> DOWN
//
// While DOWN, a connect is attempted only when the backoff schedule says it
// is due; in between, service() returns immediately, so the main loop keeps
// publishing, answering commands and reacting to shutdown.
class Bridge {
 public:
  Bridge(const cell::Json& config, cell::HeartbeatEmitter& heartbeat)
      : registers_(RegisterMap::from_json(cell::cfg_get<cell::Json>(config, "registers"))),
        max_speed_(cell::cfg_get<double>(config, "max_speed")),
        link_(cell::cfg_get<std::string>(config, "modbus.host"),
              cell::cfg_get<int>(config, "modbus.port"),
              cell::cfg_get<int>(config, "modbus.unit_id"),
              cell::cfg_get<std::uint32_t>(config, "modbus.response_timeout_ms")),
        schedule_(Backoff(
            BackoffConfig::from_json(cell::cfg_get<cell::Json>(config, "backoff")),
            cell::cfg_get_or<std::uint64_t>(config, "backoff.seed", std::random_device{}()))),
        heartbeat_(heartbeat),
        target_(cell::cfg_get<std::string>(config, "modbus.host") + ":" +
                std::to_string(cell::cfg_get<int>(config, "modbus.port"))) {
    if (!(max_speed_ > 0.0) || max_speed_ * registers_.speed_scale > 65535.0) {
      throw cell::ConfigError(
          "config key 'max_speed' must be positive and fit a 16-bit "
          "register after scaling by registers.speed_scale");
    }
    heartbeat_.set_status(cell::PROCESS_DEGRADED, "modbus link down: connecting to " + target_);
  }

  bool link_up() const noexcept { return link_.connected(); }

  // One poll cycle: (re)connect if due, then read the PLC into `state`.
  // While the link is down `state` keeps its last known values (zeros before
  // the first successful read) and only link_up changes - consumers must
  // treat the values as stale whenever link_up is false.
  void service(std::uint64_t now_ns, cell::ConveyorState& state) {
    if (!link_.connected() && schedule_.due(now_ns)) {
      if (link_.connect()) {
        schedule_.on_connected();
        announced_down_ = false;
        heartbeat_.set_status(cell::PROCESS_OK);
        spdlog::info("modbus link up: connected to {}", target_);
      } else {
        on_link_error(now_ns);
      }
    }
    if (link_.connected() && !poll(state)) {
      on_link_error(now_ns);
    }
    state.set_link_up(link_.connected());
  }

  CommandOutcome handle(const cell::Command& cmd, std::uint64_t now_ns) {
    switch (cmd.action_case()) {
      case cell::Command::kSetConveyorSpeed: {
        const double speed = cmd.set_conveyor_speed().speed();
        if (auto error = cell::plc::validate_speed(speed, max_speed_)) {
          return {false, std::move(*error)};
        }
        return write(registers_.hr_speed_setpoint,
                     cell::plc::speed_to_register(speed, registers_.speed_scale), now_ns);
      }
      case cell::Command::kFaultInject: {
        const cell::FaultInject& inject = cmd.fault_inject();
        if (inject.kind() == cell::FaultInject::RECONNECT) {
          return {false,
                  "fault_inject RECONNECT is not supported for the PLC: it is "
                  "unreachable during an outage, which ends on its own"};
        }
        if (inject.kind() != cell::FaultInject::DISCONNECT) {
          return {false, "fault_inject kind must be DISCONNECT"};
        }
        const std::uint16_t value = cell::plc::outage_register_value(inject.duration_ms());
        CommandOutcome outcome = write(registers_.hr_sim_outage_ms, value, now_ns);
        if (outcome.ok) {
          spdlog::warn("fault injection: asked the PLC to drop off the network ({})",
                       value == cell::plc::kOutageUntilRestart
                           ? std::string("until it is restarted")
                           : std::to_string(value) + " ms");
        }
        return outcome;
      }
      case cell::Command::ACTION_NOT_SET:
        return {false, "command has no action"};
      default:
        return {false,
                "unsupported action for plc: only set_conveyor_speed and fault_inject "
                "are implemented"};
    }
  }

 private:
  bool poll(cell::ConveyorState& state) {
    const int lo = std::min(registers_.ir_speed_actual, registers_.ir_fault_code);
    const int hi = std::max(registers_.ir_speed_actual, registers_.ir_fault_code);
    std::uint16_t speed = 0;
    std::uint16_t fault = 0;
    if (hi - lo < kMaxRegisterSpan) {
      std::array<std::uint16_t, kMaxRegisterSpan> regs{};
      const auto count = static_cast<std::size_t>(hi - lo + 1);
      if (!link_.read_input_registers(lo, std::span(regs).first(count))) {
        return false;
      }
      speed = regs[static_cast<std::size_t>(registers_.ir_speed_actual - lo)];
      fault = regs[static_cast<std::size_t>(registers_.ir_fault_code - lo)];
    } else if (!link_.read_input_registers(registers_.ir_speed_actual, {&speed, 1}) ||
               !link_.read_input_registers(registers_.ir_fault_code, {&fault, 1})) {
      return false;
    }
    bool item_present = false;
    if (!link_.read_discrete_input(registers_.di_item_present, item_present)) {
      return false;
    }
    state.set_speed(cell::plc::register_to_speed(speed, registers_.speed_scale));
    state.set_fault_code(fault);
    state.set_item_present(item_present);
    return true;
  }

  CommandOutcome write(int address, std::uint16_t value, std::uint64_t now_ns) {
    if (!link_.connected()) {
      return {false, "PLC link is down"};
    }
    if (!link_.write_register(address, value)) {
      CommandOutcome outcome{false, "PLC write failed: " + link_.last_error()};
      on_link_error(now_ns);
      return outcome;
    }
    return {};
  }

  // The link just failed (ModbusLink has already closed it): schedule the
  // next attempt and report. Logged at warn once per outage, not per retry.
  void on_link_error(std::uint64_t now_ns) {
    const std::uint64_t delay_ms = schedule_.on_failure(now_ns);
    if (!announced_down_) {
      spdlog::warn("modbus link down ({}): {}; reconnecting with backoff, first retry in {} ms",
                   target_, link_.last_error(), delay_ms);
      announced_down_ = true;
    } else {
      spdlog::debug("modbus reconnect failed: {}; next retry in {} ms", link_.last_error(),
                    delay_ms);
    }
    heartbeat_.set_status(cell::PROCESS_DEGRADED, "modbus link down: " + link_.last_error());
  }

  RegisterMap registers_;
  double max_speed_;
  ModbusLink link_;
  ReconnectSchedule schedule_;
  cell::HeartbeatEmitter& heartbeat_;
  std::string target_;
  bool announced_down_ = false;
};

// Answer every waiting command without blocking; every request gets a reply.
void drain_commands(zmq::socket_t& rep, Bridge& bridge, std::uint64_t& reply_seq) {
  std::vector<zmq::message_t> frames;
  while (cell::recv_multipart(rep, frames, /*dontwait=*/true)) {
    cell::Command cmd;
    CommandOutcome outcome;
    if (frames.size() != 1 || !cell::parse_proto(frames.front(), cmd)) {
      outcome = {false, "malformed command: expected one Command frame"};
    } else {
      outcome = bridge.handle(cmd, cell::mono_now_ns());
    }
    if (!outcome.ok) {
      spdlog::warn("command rejected: {}", outcome.error);
    }
    cell::CommandReply reply;
    reply.set_ok(outcome.ok);
    reply.set_error(outcome.error);
    cell::fill_header(*reply.mutable_h(), reply_seq++, cell::process::kPlcBridge);
    cell::send_proto(rep, reply);
  }
}

int run(const cell::Json& config) {
  const auto telemetry_endpoint = cell::cfg_get<std::string>(config, "telemetry_endpoint");
  const auto cmd_endpoint = cell::cfg_get<std::string>(config, "cmd_endpoint");
  const auto poll_hz = cell::cfg_get<double>(config, "poll_hz");
  const auto heartbeat_ms = cell::cfg_get<std::uint64_t>(config, "heartbeat_interval_ms");
  if (!(poll_hz > 0.0) || poll_hz > 10'000.0) {
    throw cell::ConfigError("config key 'poll_hz' must be in (0, 10000]");
  }

  cell::HeartbeatEmitter heartbeat(cell::process::kPlcBridge, heartbeat_ms);
  Bridge bridge(config, heartbeat);

  zmq::context_t ctx(1);
  zmq::socket_t pub = cell::make_pub(ctx, telemetry_endpoint);
  zmq::socket_t rep = cell::make_rep(ctx, cmd_endpoint);

  cell::Watchdog watchdog;
  watchdog.ready();
  spdlog::info("plc_bridge running: polling at {} Hz, telemetry {}, commands {}", poll_hz,
               telemetry_endpoint, cmd_endpoint);

  const auto period_ns = static_cast<std::uint64_t>(static_cast<double>(kNsPerSec) / poll_hz);
  cell::ConveyorState state;
  std::uint64_t seq = 0;
  std::uint64_t reply_seq = 0;
  std::uint64_t deadline = cell::mono_now_ns() + period_ns;

  while (!cell::shutdown_requested()) {
    const cell::ProcessStatus status_before = heartbeat.status();
    drain_commands(rep, bridge, reply_seq);
    bridge.service(cell::mono_now_ns(), state);

    // Published every cycle, link up or not, so the stream and its sequence
    // numbers stay continuous and consumers see the outage as link_up=false.
    cell::fill_header(*state.mutable_h(), seq, cell::process::kPlcBridge);
    if (cell::send_proto(pub, cell::topic::kPlcConveyor, state)) {
      ++seq;
    }

    const std::uint64_t now = cell::mono_now_ns();
    if (heartbeat.status() != status_before) {
      heartbeat.publish_now(pub, now);  // make link transitions visible immediately
    }
    heartbeat.tick(pub, now);
    watchdog.kick(now);

    // Absolute deadlines (no drift). A Modbus timeout can cost several
    // periods; resynchronise afterwards instead of polling in a burst.
    deadline += period_ns;
    if (now > deadline + period_ns) {
      deadline = now + period_ns;
    }
    cell::sleep_until_ns(deadline);
  }

  watchdog.stopping();
  heartbeat.set_status(cell::PROCESS_STOPPING);
  heartbeat.publish_now(pub, cell::mono_now_ns());
  spdlog::info("plc_bridge stopped cleanly: {} conveyor states published, link was {}", seq,
               bridge.link_up() ? "up" : "down");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  return cell::run_app(argc, argv,
                       {cell::process::kPlcBridge, "Modbus-TCP bridge to the conveyor PLC"}, run);
}
