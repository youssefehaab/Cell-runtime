#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "arm_sim/motion_profile.hpp"
#include "cell.pb.h"
#include "common/config.hpp"

namespace cell::arm {

struct ArmConfig {
  std::size_t joints = 6;
  std::vector<double> home;  // start pose, rad; size == joints
  double joint_limit_rad = 0.0;
  double max_velocity = 0.0;
  double max_acceleration = 0.0;
  double estop_deceleration = 0.0;

  // Reads the arm_sim config keys ("home" defaults to all zeros) and
  // validates them. Throws ConfigError.
  static ArmConfig from_json(const Json& config);
  void validate() const;
};

struct CommandResult {
  bool ok = true;
  std::string error;

  static CommandResult success() { return {}; }
  static CommandResult failure(std::string why) { return {false, std::move(why)}; }
};

// Arm state machine + per-joint motion profiles. Pure and deterministic (no
// clock, no I/O): feed it commands and step(dt).
//
//   IDLE --MoveTo--> MOVING --arrived--> IDLE
//   MOVING/IDLE --Stop--> STOPPING --at rest--> IDLE   (target discarded)
//   any --EStop--> ESTOP (latched) --Reset, once at rest--> IDLE
//
// Joints move independently: each runs its own time-optimal profile under the
// shared per-joint limits, so joints with shorter moves finish earlier.
class ArmModel {
 public:
  explicit ArmModel(ArmConfig config);

  // Dispatch on the Command's action. Actions the arm does not implement and
  // commands with no action are rejected.
  CommandResult handle(const Command& cmd);

  CommandResult move_to(std::span<const double> joints);
  CommandResult stop();
  CommandResult estop();
  CommandResult reset();

  void step(double dt);

  ArmMode mode() const noexcept { return mode_; }
  std::size_t joints() const noexcept { return axes_.size(); }
  double position(std::size_t joint) const { return axes_.at(joint).position(); }
  double velocity(std::size_t joint) const { return axes_.at(joint).velocity(); }
  bool at_rest() const noexcept;

  // Write positions, velocities and mode (not the header) into `out`.
  void fill(JointState& out) const;

 private:
  ArmConfig config_;
  AxisLimits limits_;
  std::vector<AxisProfile> axes_;
  ArmMode mode_ = ARM_IDLE;
};

}  // namespace cell::arm
