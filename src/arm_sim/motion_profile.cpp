#include "arm_sim/motion_profile.hpp"

#include <algorithm>
#include <cmath>

namespace cell::arm {
namespace {

// Positions closer than this are the same position (1 nrad). It only has to
// absorb floating-point residue from the closed-form phase times.
constexpr double kPositionEpsilon = 1e-9;

// A tick can span at most: brake (wrong way) -> accelerate -> cruise ->
// decelerate -> arrive. The bound is a guard against a non-terminating loop on
// pathological input, not something normal motion gets near.
constexpr int kMaxPhasesPerStep = 8;

enum class PhaseEnd { kNone, kAtRest, kAtMaxVelocity };

}  // namespace

void AxisProfile::advance(double t, double acceleration) noexcept {
  pos_ += vel_ * t + 0.5 * acceleration * t * t;
  vel_ += acceleration * t;
}

void AxisProfile::step(double dt, const AxisLimits& limits) noexcept {
  const double a = limits.max_acceleration;
  const double vmax = limits.max_velocity;
  double remaining = dt;

  for (int phase = 0; phase < kMaxPhasesPerStep && remaining > 0.0; ++phase) {
    if (vel_ == 0.0 && std::abs(target_ - pos_) <= kPositionEpsilon) {
      pos_ = target_;
      return;
    }

    // Where the axis would come to rest if it started braking right now.
    const double stop_pos = pos_ + vel_ * std::abs(vel_) / (2.0 * a);
    const double stop_error = target_ - stop_pos;

    if (std::abs(stop_error) <= kPositionEpsilon) {
      // Final deceleration: braking now lands on the target.
      const double t_stop = std::abs(vel_) / a;
      if (t_stop <= remaining) {
        pos_ = target_;
        vel_ = 0.0;
        return;
      }
      advance(remaining, vel_ > 0.0 ? -a : a);
      return;
    }

    // Direction we must accelerate in, and our speed along it. A negative
    // speed means we are moving away from, or would overshoot, the target.
    const double dir = stop_error > 0.0 ? 1.0 : -1.0;
    const double speed = vel_ * dir;

    double acceleration = 0.0;
    double t_phase = 0.0;
    PhaseEnd end = PhaseEnd::kNone;
    if (speed < 0.0) {
      acceleration = dir * a;
      t_phase = -speed / a;
      end = PhaseEnd::kAtRest;
    } else if (speed > vmax) {
      // Only reachable if the limits were lowered mid-move.
      acceleration = -dir * a;
      t_phase = (speed - vmax) / a;
      end = PhaseEnd::kAtMaxVelocity;
    } else {
      const double distance = (target_ - pos_) * dir;  // > 0
      if (speed < vmax) {
        // Accelerate until either vmax or the point where the braking
        // distance equals the distance left: solve
        //   distance - (speed*t + a*t^2/2) = (speed + a*t)^2 / (2a)   for t.
        const double t_switch = (-speed + std::sqrt(0.5 * speed * speed + a * distance)) / a;
        const double t_vmax = (vmax - speed) / a;
        acceleration = dir * a;
        if (t_vmax <= t_switch) {
          t_phase = t_vmax;
          end = PhaseEnd::kAtMaxVelocity;
        } else {
          t_phase = t_switch;
        }
      } else {
        // Cruise until the braking distance equals the distance left.
        t_phase = (distance - vmax * vmax / (2.0 * a)) / vmax;
      }
    }

    t_phase = std::max(t_phase, 0.0);
    if (t_phase > remaining) {
      advance(remaining, acceleration);
      return;
    }
    advance(t_phase, acceleration);
    remaining -= t_phase;
    // Land exactly on the phase boundary so rounding residue cannot leave a
    // tiny wrong-signed or over-limit velocity behind.
    if (end == PhaseEnd::kAtRest) {
      vel_ = 0.0;
    } else if (end == PhaseEnd::kAtMaxVelocity) {
      vel_ = dir * vmax;
    }
  }
}

void AxisProfile::brake(double dt, double deceleration) noexcept {
  if (vel_ == 0.0) {
    return;
  }
  const double t_stop = std::abs(vel_) / deceleration;
  if (t_stop <= dt) {
    pos_ += vel_ * std::abs(vel_) / (2.0 * deceleration);
    vel_ = 0.0;
    return;
  }
  advance(dt, vel_ > 0.0 ? -deceleration : deceleration);
}

}  // namespace cell::arm
