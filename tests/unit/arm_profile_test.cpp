#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <tuple>

#include "arm_sim/motion_profile.hpp"

namespace {

using cell::arm::AxisLimits;
using cell::arm::AxisProfile;

constexpr double kDt = 0.001;
constexpr AxisLimits kLimits{1.5, 4.0};

struct RunResult {
  int ticks = 0;            // ticks until at_target()
  double peak_speed = 0.0;  // max |v| seen
  double peak_accel = 0.0;  // max |dv|/dt seen
  double min_pos = 0.0;
  double max_pos = 0.0;
  bool reversed = false;  // velocity changed sign during the run
};

// Step until arrival (or max_ticks), checking the limits on every tick.
RunResult run_to_target(AxisProfile& axis, const AxisLimits& limits, double dt, int max_ticks) {
  RunResult r;
  r.min_pos = r.max_pos = axis.position();
  double last_sign = 0.0;
  while (!axis.at_target() && r.ticks < max_ticks) {
    const double v0 = axis.velocity();
    axis.step(dt, limits);
    ++r.ticks;
    r.peak_speed = std::max(r.peak_speed, std::abs(axis.velocity()));
    r.peak_accel = std::max(r.peak_accel, std::abs(axis.velocity() - v0) / dt);
    r.min_pos = std::min(r.min_pos, axis.position());
    r.max_pos = std::max(r.max_pos, axis.position());
    const double sign = axis.velocity() > 0.0 ? 1.0 : (axis.velocity() < 0.0 ? -1.0 : 0.0);
    if (sign != 0.0 && last_sign != 0.0 && sign != last_sign) {
      r.reversed = true;
    }
    if (sign != 0.0) {
      last_sign = sign;
    }
  }
  return r;
}

void expect_within_limits(const RunResult& r, const AxisLimits& limits) {
  EXPECT_LE(r.peak_speed, limits.max_velocity + 1e-12);
  EXPECT_LE(r.peak_accel, limits.max_acceleration + 1e-6);
}

TEST(ArmProfile, StartsAtRestOnTarget) {
  AxisProfile axis(0.25);
  EXPECT_TRUE(axis.at_target());
  axis.step(kDt, kLimits);
  EXPECT_DOUBLE_EQ(axis.position(), 0.25);
  EXPECT_DOUBLE_EQ(axis.velocity(), 0.0);
}

TEST(ArmProfile, LongMoveIsTrapezoidalAndLandsExactly) {
  AxisProfile axis(0.0);
  axis.set_target(2.0);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);

  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), 2.0);  // exact, not approximately
  EXPECT_EQ(axis.velocity(), 0.0);
  expect_within_limits(r, kLimits);
  EXPECT_NEAR(r.peak_speed, kLimits.max_velocity, 1e-12);  // reaches the cruise phase
  EXPECT_LE(r.max_pos, 2.0);                               // no overshoot
  EXPECT_FALSE(r.reversed);

  // Time-optimal duration: d/v + v/a.
  const double expected_s = 2.0 / 1.5 + 1.5 / 4.0;
  EXPECT_NEAR(r.ticks * kDt, expected_s, 2 * kDt);
}

TEST(ArmProfile, ShortMoveIsTriangular) {
  AxisProfile axis(0.0);
  axis.set_target(0.1);  // too short to reach max_velocity (needs 0.5625 rad)
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);

  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), 0.1);
  expect_within_limits(r, kLimits);
  // Peak of a triangular profile: sqrt(a*d).
  EXPECT_NEAR(r.peak_speed, std::sqrt(4.0 * 0.1), 0.01);
  EXPECT_LT(r.peak_speed, kLimits.max_velocity);
  EXPECT_LE(r.max_pos, 0.1);
  EXPECT_NEAR(r.ticks * kDt, 2.0 * std::sqrt(0.1 / 4.0), 2 * kDt);
}

TEST(ArmProfile, NegativeDirectionMirrorsPositive) {
  AxisProfile axis(1.0);
  axis.set_target(-1.0);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), -1.0);
  expect_within_limits(r, kLimits);
  EXPECT_GE(r.min_pos, -1.0);
  EXPECT_LE(r.max_pos, 1.0);
}

TEST(ArmProfile, MoveSmallerThanOneTickCompletes) {
  AxisProfile axis(0.0);
  axis.set_target(1e-7);  // takes 2*sqrt(d/a) = 0.3 ms, less than one tick
  axis.step(kDt, kLimits);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), 1e-7);
}

TEST(ArmProfile, StaysPutAfterArrival) {
  AxisProfile axis(0.0);
  axis.set_target(0.3);
  run_to_target(axis, kLimits, kDt, 100'000);
  ASSERT_TRUE(axis.at_target());
  for (int i = 0; i < 1000; ++i) {
    axis.step(kDt, kLimits);
    ASSERT_EQ(axis.position(), 0.3);
    ASSERT_EQ(axis.velocity(), 0.0);  // no chatter around the target
  }
}

TEST(ArmProfile, RetargetFurtherWhileMovingKeepsGoing) {
  AxisProfile axis(0.0);
  axis.set_target(1.0);
  for (int i = 0; i < 300; ++i) {
    axis.step(kDt, kLimits);
  }
  ASSERT_GT(axis.velocity(), 0.0);
  axis.set_target(2.5);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), 2.5);
  expect_within_limits(r, kLimits);
  EXPECT_FALSE(r.reversed);
  EXPECT_LE(r.max_pos, 2.5);
}

TEST(ArmProfile, RetargetBehindReversesWithinLimits) {
  AxisProfile axis(0.0);
  axis.set_target(2.0);
  for (int i = 0; i < 500; ++i) {  // now cruising at max velocity
    axis.step(kDt, kLimits);
  }
  ASSERT_DOUBLE_EQ(axis.velocity(), kLimits.max_velocity);
  const double pos_at_retarget = axis.position();

  axis.set_target(-0.5);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), -0.5);
  expect_within_limits(r, kLimits);
  EXPECT_TRUE(r.reversed);
  // It cannot stop instantly: it carries on for exactly the braking distance.
  const double braking = kLimits.max_velocity * kLimits.max_velocity / (2 * 4.0);
  EXPECT_NEAR(r.max_pos, pos_at_retarget + braking, 1e-6);
  EXPECT_GE(r.min_pos, -0.5);  // and does not overshoot the new target
}

TEST(ArmProfile, RetargetCloserThanBrakingDistanceOvershootsThenReturns) {
  AxisProfile axis(0.0);
  axis.set_target(2.0);
  for (int i = 0; i < 500; ++i) {
    axis.step(kDt, kLimits);
  }
  const double target = axis.position() + 0.05;  // braking needs 0.28 rad
  axis.set_target(target);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), target);
  expect_within_limits(r, kLimits);
  EXPECT_GT(r.max_pos, target);  // physically unavoidable overshoot...
  EXPECT_TRUE(r.reversed);       // ...then it comes back
}

TEST(ArmProfile, RetargetDuringFinalDeceleration) {
  AxisProfile axis(0.0);
  axis.set_target(0.5);
  while (axis.position() < 0.45) {
    axis.step(kDt, kLimits);
  }
  axis.set_target(0.2);
  const RunResult r = run_to_target(axis, kLimits, kDt, 100'000);
  EXPECT_TRUE(axis.at_target());
  EXPECT_EQ(axis.position(), 0.2);
  expect_within_limits(r, kLimits);
}

TEST(ArmProfile, BrakeStopsAtBrakingDistanceAndStays) {
  AxisProfile axis(0.0);
  axis.set_target(2.0);
  for (int i = 0; i < 500; ++i) {
    axis.step(kDt, kLimits);
  }
  const double p0 = axis.position();
  const double v0 = axis.velocity();
  const double decel = 20.0;
  int ticks = 0;
  while (!axis.at_rest() && ticks < 10'000) {
    const double before = axis.velocity();
    axis.brake(kDt, decel);
    EXPECT_LE(std::abs(axis.velocity() - before) / kDt, decel + 1e-6);
    EXPECT_LE(axis.velocity(), before);  // monotonic, never reverses
    EXPECT_GE(axis.velocity(), 0.0);
    ++ticks;
  }
  EXPECT_TRUE(axis.at_rest());
  EXPECT_NEAR(axis.position(), p0 + v0 * v0 / (2 * decel), 1e-9);
  EXPECT_NEAR(ticks * kDt, v0 / decel, 2 * kDt);

  const double rest = axis.position();
  axis.brake(kDt, decel);
  EXPECT_EQ(axis.position(), rest);
  axis.hold();
  EXPECT_TRUE(axis.at_target());
}

TEST(ArmProfile, IsDeterministic) {
  AxisProfile a(0.1);
  AxisProfile b(0.1);
  a.set_target(-1.3);
  b.set_target(-1.3);
  for (int i = 0; i < 3000; ++i) {
    a.step(kDt, kLimits);
    b.step(kDt, kLimits);
    ASSERT_EQ(a.position(), b.position());
    ASSERT_EQ(a.velocity(), b.velocity());
  }
}

// Sweep distances, limits and tick sizes: every move must land exactly, stay
// inside the limits and take the time-optimal duration.
class ArmProfileSweep
    : public ::testing::TestWithParam<std::tuple<double, double, double, double>> {};

TEST_P(ArmProfileSweep, LandsExactlyWithinLimits) {
  const auto [distance, vmax, amax, dt] = GetParam();
  const AxisLimits limits{vmax, amax};
  for (const double start : {0.0, -0.7, 1.234567}) {
    for (const double sign : {1.0, -1.0}) {
      AxisProfile axis(start);
      const double target = start + sign * distance;
      axis.set_target(target);
      const RunResult r = run_to_target(axis, limits, dt, 10'000'000);
      ASSERT_TRUE(axis.at_target()) << "d=" << distance << " start=" << start;
      EXPECT_EQ(axis.position(), target);
      expect_within_limits(r, limits);
      EXPECT_FALSE(r.reversed);
      EXPECT_LE(r.max_pos, std::max(start, target) + 1e-12);
      EXPECT_GE(r.min_pos, std::min(start, target) - 1e-12);

      const double d_accel = vmax * vmax / amax;  // distance to reach vmax and brake again
      const double expected_s =
          distance >= d_accel ? distance / vmax + vmax / amax : 2.0 * std::sqrt(distance / amax);
      EXPECT_NEAR(r.ticks * dt, expected_s, 2 * dt);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Moves, ArmProfileSweep,
    ::testing::Combine(::testing::Values(1e-6, 0.001, 0.0371, 0.5625, 1.0, 3.14159, 6.0),
                       ::testing::Values(0.5, 1.5), ::testing::Values(4.0, 20.0),
                       ::testing::Values(0.001, 0.01, 0.000137)));

}  // namespace
