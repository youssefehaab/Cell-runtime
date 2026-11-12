#pragma once

namespace cell::arm {

struct AxisLimits {
  double max_velocity = 0.0;      // rad/s, > 0
  double max_acceleration = 0.0;  // rad/s^2, > 0
};

// Time-optimal trapezoidal (or triangular, for short moves) velocity profile
// for one axis.
//
// Pure and deterministic: no clock, no I/O. The profile is not precomputed;
// every step() re-derives the remaining motion from the current
// (position, velocity, target), which is what makes retargeting mid-move -
// including a reversal - a non-event: set_target() and keep stepping.
//
// Each step integrates the piecewise-constant-acceleration motion exactly,
// splitting the tick at phase boundaries (accelerate / cruise / decelerate),
// so the axis arrives on the target with zero velocity instead of hunting
// around it the way a per-tick "accelerate or brake?" controller does.
class AxisProfile {
 public:
  explicit AxisProfile(double position = 0.0) : pos_(position), target_(position) {}

  void set_target(double target) noexcept { target_ = target; }
  // Make the current position the target (used after a stop).
  void hold() noexcept { target_ = pos_; }

  // Advance `dt` seconds toward the target within `limits`.
  void step(double dt, const AxisLimits& limits) noexcept;

  // Advance `dt` seconds while decelerating to rest at `deceleration`,
  // ignoring the target.
  void brake(double dt, double deceleration) noexcept;

  double position() const noexcept { return pos_; }
  double velocity() const noexcept { return vel_; }
  double target() const noexcept { return target_; }
  bool at_rest() const noexcept { return vel_ == 0.0; }
  bool at_target() const noexcept { return vel_ == 0.0 && pos_ == target_; }

 private:
  void advance(double t, double acceleration) noexcept;

  double pos_;
  double vel_ = 0.0;
  double target_;
};

}  // namespace cell::arm
