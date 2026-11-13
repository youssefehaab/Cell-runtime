#include "arm_sim/arm_model.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cell::arm {
namespace {

void require_positive(double value, const char* key) {
  if (!std::isfinite(value) || value <= 0.0) {
    throw ConfigError(std::string("config key '") + key + "' must be a positive number");
  }
}

}  // namespace

ArmConfig ArmConfig::from_json(const Json& config) {
  ArmConfig c;
  c.joints = cfg_get<std::size_t>(config, "joints");
  c.home = cfg_get_or<std::vector<double>>(config, "home", std::vector<double>(c.joints, 0.0));
  c.joint_limit_rad = cfg_get<double>(config, "joint_limit_rad");
  c.max_velocity = cfg_get<double>(config, "max_velocity");
  c.max_acceleration = cfg_get<double>(config, "max_acceleration");
  c.estop_deceleration = cfg_get<double>(config, "estop_deceleration");
  c.validate();
  return c;
}

void ArmConfig::validate() const {
  if (joints == 0) {
    throw ConfigError("config key 'joints' must be at least 1");
  }
  require_positive(joint_limit_rad, "joint_limit_rad");
  require_positive(max_velocity, "max_velocity");
  require_positive(max_acceleration, "max_acceleration");
  require_positive(estop_deceleration, "estop_deceleration");
  if (home.size() != joints) {
    throw ConfigError("config key 'home' has " + std::to_string(home.size()) +
                      " entries but 'joints' is " + std::to_string(joints));
  }
  for (const double h : home) {
    if (!std::isfinite(h) || std::abs(h) > joint_limit_rad) {
      throw ConfigError("config key 'home' has an entry outside +/- joint_limit_rad");
    }
  }
}

ArmModel::ArmModel(ArmConfig config) : config_(std::move(config)) {
  config_.validate();
  limits_ = AxisLimits{config_.max_velocity, config_.max_acceleration};
  axes_.reserve(config_.joints);
  for (const double h : config_.home) {
    axes_.emplace_back(h);
  }
}

CommandResult ArmModel::handle(const Command& cmd) {
  switch (cmd.action_case()) {
    case Command::kMoveTo: {
      const auto& joints = cmd.move_to().joints();
      return move_to({joints.data(), static_cast<std::size_t>(joints.size())});
    }
    case Command::kStop:
      return stop();
    case Command::kEstop:
      return estop();
    case Command::kReset:
      return reset();
    case Command::kSetConveyorSpeed:
      return CommandResult::failure("unsupported action for arm: set_conveyor_speed");
    case Command::kFaultInject:
      return CommandResult::failure("unsupported action for arm: fault_inject");
    case Command::ACTION_NOT_SET:
      return CommandResult::failure("command has no action");
  }
  return CommandResult::failure("unknown action");
}

CommandResult ArmModel::move_to(std::span<const double> joints) {
  if (mode_ == ARM_ESTOP) {
    return CommandResult::failure("arm is in ESTOP; send reset first");
  }
  if (joints.size() != axes_.size()) {
    return CommandResult::failure("move_to has " + std::to_string(joints.size()) +
                                  " joints, arm has " + std::to_string(axes_.size()));
  }
  for (std::size_t i = 0; i < joints.size(); ++i) {
    if (!std::isfinite(joints[i])) {
      return CommandResult::failure("move_to joint " + std::to_string(i) + " is not finite");
    }
    if (std::abs(joints[i]) > config_.joint_limit_rad) {
      return CommandResult::failure("move_to joint " + std::to_string(i) + " target " +
                                    std::to_string(joints[i]) + " rad exceeds limit +/-" +
                                    std::to_string(config_.joint_limit_rad) + " rad");
    }
  }
  for (std::size_t i = 0; i < joints.size(); ++i) {
    axes_[i].set_target(joints[i]);
  }
  mode_ = ARM_MOVING;
  return CommandResult::success();
}

CommandResult ArmModel::stop() {
  // ESTOP already brakes harder and stays latched; IDLE has nothing to stop.
  if (mode_ == ARM_MOVING) {
    mode_ = ARM_STOPPING;
  }
  return CommandResult::success();
}

CommandResult ArmModel::estop() {
  mode_ = ARM_ESTOP;
  return CommandResult::success();
}

CommandResult ArmModel::reset() {
  if (mode_ != ARM_ESTOP) {
    return CommandResult::success();  // nothing latched: idempotent no-op
  }
  if (!at_rest()) {
    return CommandResult::failure("cannot reset: arm is still decelerating");
  }
  mode_ = ARM_IDLE;
  return CommandResult::success();
}

bool ArmModel::at_rest() const noexcept {
  return std::all_of(axes_.begin(), axes_.end(), [](const auto& a) { return a.at_rest(); });
}

void ArmModel::step(double dt) {
  switch (mode_) {
    case ARM_MOVING: {
      bool arrived = true;
      for (auto& axis : axes_) {
        axis.step(dt, limits_);
        arrived = arrived && axis.at_target();
      }
      if (arrived) {
        mode_ = ARM_IDLE;
      }
      break;
    }
    case ARM_STOPPING:
    case ARM_ESTOP: {
      const double decel =
          mode_ == ARM_ESTOP ? config_.estop_deceleration : config_.max_acceleration;
      for (auto& axis : axes_) {
        axis.brake(dt, decel);
        if (axis.at_rest()) {
          axis.hold();  // discard the old target
        }
      }
      if (mode_ == ARM_STOPPING && at_rest()) {
        mode_ = ARM_IDLE;
      }
      break;
    }
    default:
      break;  // IDLE: hold position
  }
}

void ArmModel::fill(JointState& out) const {
  const int n = static_cast<int>(axes_.size());
  out.mutable_position()->Resize(n, 0.0);
  out.mutable_velocity()->Resize(n, 0.0);
  for (int i = 0; i < n; ++i) {
    out.set_position(i, axes_[static_cast<std::size_t>(i)].position());
    out.set_velocity(i, axes_[static_cast<std::size_t>(i)].velocity());
  }
  out.set_mode(mode_);
}

}  // namespace cell::arm
