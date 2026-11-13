#include "arm_sim/arm_model.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "cell.pb.h"
#include "common/config.hpp"

namespace {

using cell::arm::ArmConfig;
using cell::arm::ArmModel;

constexpr double kDt = 0.001;

ArmConfig test_config() {
  ArmConfig c;
  c.joints = 6;
  c.home = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  c.joint_limit_rad = 3.14159;
  c.max_velocity = 1.5;
  c.max_acceleration = 4.0;
  c.estop_deceleration = 20.0;
  return c;
}

// Step until the mode leaves `while_mode` (or the tick budget runs out).
int step_while(ArmModel& arm, cell::ArmMode while_mode, int max_ticks = 100'000) {
  int ticks = 0;
  while (arm.mode() == while_mode && ticks < max_ticks) {
    arm.step(kDt);
    ++ticks;
  }
  return ticks;
}

double max_speed(const ArmModel& arm) {
  double v = 0.0;
  for (std::size_t i = 0; i < arm.joints(); ++i) {
    v = std::max(v, std::abs(arm.velocity(i)));
  }
  return v;
}

TEST(ArmModel, StartsIdleAtHome) {
  ArmConfig c = test_config();
  c.home = {0.1, -0.2, 0.3, 0.0, 0.5, -0.6};
  ArmModel arm(c);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_TRUE(arm.at_rest());
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_DOUBLE_EQ(arm.position(i), c.home[i]);
  }
  arm.step(kDt);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_DOUBLE_EQ(arm.position(1), -0.2);
}

TEST(ArmModel, MoveToReachesTargetAndReturnsToIdle) {
  ArmModel arm(test_config());
  const std::vector<double> target{0.5, -0.4, 0.3, 1.0, -1.2, 0.05};
  ASSERT_TRUE(arm.move_to(target).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_MOVING);

  step_while(arm, cell::ARM_MOVING);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  for (std::size_t i = 0; i < target.size(); ++i) {
    EXPECT_EQ(arm.position(i), target[i]) << "joint " << i;
    EXPECT_EQ(arm.velocity(i), 0.0);
  }
}

TEST(ArmModel, StaysMovingUntilSlowestJointArrives) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{0.01, 0, 0, 0, 0, 2.0}).ok);
  for (int i = 0; i < 500; ++i) {
    arm.step(kDt);
  }
  EXPECT_EQ(arm.position(0), 0.01);  // short move long finished
  EXPECT_EQ(arm.mode(), cell::ARM_MOVING);
  step_while(arm, cell::ARM_MOVING);
  EXPECT_EQ(arm.position(5), 2.0);
}

TEST(ArmModel, MoveToValidation) {
  ArmModel arm(test_config());

  auto r = arm.move_to(std::vector<double>{0.1, 0.2, 0.3});
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("3 joints"), std::string::npos) << r.error;

  r = arm.move_to(std::vector<double>{0, 0, 0, 0, 0, 3.2});
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("limit"), std::string::npos) << r.error;

  r = arm.move_to(std::vector<double>{0, -3.2, 0, 0, 0, 0});
  EXPECT_FALSE(r.ok);

  r = arm.move_to(std::vector<double>{0, 0, std::nan(""), 0, 0, 0});
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("finite"), std::string::npos) << r.error;

  r = arm.move_to(std::vector<double>{});
  EXPECT_FALSE(r.ok);

  // A rejected command changes nothing.
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  arm.step(kDt);
  EXPECT_TRUE(arm.at_rest());

  // Exactly on the limit is allowed.
  EXPECT_TRUE(arm.move_to(std::vector<double>{3.14159, 0, 0, 0, 0, -3.14159}).ok);
}

TEST(ArmModel, RetargetWhileMoving) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{1.0, 1.0, 0, 0, 0, 0}).ok);
  for (int i = 0; i < 400; ++i) {
    arm.step(kDt);
  }
  ASSERT_EQ(arm.mode(), cell::ARM_MOVING);
  ASSERT_TRUE(arm.move_to(std::vector<double>{-1.0, 2.0, 0, 0, 0, 0.5}).ok);
  int ticks = 0;
  while (arm.mode() == cell::ARM_MOVING && ticks++ < 100'000) {
    arm.step(kDt);
    ASSERT_LE(max_speed(arm), 1.5 + 1e-12);
  }
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_EQ(arm.position(0), -1.0);
  EXPECT_EQ(arm.position(1), 2.0);
  EXPECT_EQ(arm.position(5), 0.5);
}

TEST(ArmModel, StopDeceleratesToRestAndClearsTarget) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{2.0, 0, 0, 0, 0, 0}).ok);
  for (int i = 0; i < 500; ++i) {
    arm.step(kDt);
  }
  const double v0 = arm.velocity(0);
  ASSERT_GT(v0, 0.0);

  ASSERT_TRUE(arm.stop().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_STOPPING);
  const int ticks = step_while(arm, cell::ARM_STOPPING);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_TRUE(arm.at_rest());
  EXPECT_NEAR(ticks * kDt, v0 / 4.0, 2 * kDt);  // braked at max_acceleration
  EXPECT_LT(arm.position(0), 2.0);

  // Target cleared: it does not resume the interrupted move.
  const double rest = arm.position(0);
  for (int i = 0; i < 1000; ++i) {
    arm.step(kDt);
  }
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_EQ(arm.position(0), rest);

  // And a fresh MoveTo works from there.
  ASSERT_TRUE(arm.move_to(std::vector<double>{0, 0, 0, 0, 0, 0}).ok);
  step_while(arm, cell::ARM_MOVING);
  EXPECT_EQ(arm.position(0), 0.0);
}

TEST(ArmModel, StopWhenIdleIsANoOp) {
  ArmModel arm(test_config());
  EXPECT_TRUE(arm.stop().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
}

TEST(ArmModel, MoveToDuringStoppingRetargets) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{2.0, 0, 0, 0, 0, 0}).ok);
  for (int i = 0; i < 500; ++i) {
    arm.step(kDt);
  }
  ASSERT_TRUE(arm.stop().ok);
  arm.step(kDt);
  ASSERT_EQ(arm.mode(), cell::ARM_STOPPING);
  ASSERT_TRUE(arm.move_to(std::vector<double>{0.2, 0, 0, 0, 0, 0}).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_MOVING);
  step_while(arm, cell::ARM_MOVING);
  EXPECT_EQ(arm.position(0), 0.2);
}

TEST(ArmModel, EStopLatchesUntilReset) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{2.0, -2.0, 0, 0, 0, 0}).ok);
  for (int i = 0; i < 500; ++i) {
    arm.step(kDt);
  }
  const double v0 = max_speed(arm);

  ASSERT_TRUE(arm.estop().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);

  // Reset is refused while still decelerating.
  auto r = arm.reset();
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("decelerating"), std::string::npos) << r.error;
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);

  int ticks = 0;
  while (!arm.at_rest() && ticks < 100'000) {
    arm.step(kDt);
    ++ticks;
  }
  EXPECT_NEAR(ticks * kDt, v0 / 20.0, 2 * kDt);  // braked at estop_deceleration
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);        // at rest but still latched

  // Latched: motion commands are refused, Stop is harmless.
  r = arm.move_to(std::vector<double>{0, 0, 0, 0, 0, 0});
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("ESTOP"), std::string::npos) << r.error;
  EXPECT_TRUE(arm.stop().ok);
  for (int i = 0; i < 100; ++i) {
    arm.step(kDt);
  }
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);
  EXPECT_TRUE(arm.at_rest());

  const double rest = arm.position(0);
  ASSERT_TRUE(arm.reset().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  for (int i = 0; i < 100; ++i) {
    arm.step(kDt);
  }
  EXPECT_EQ(arm.position(0), rest);  // the pre-estop target is gone

  ASSERT_TRUE(arm.move_to(std::vector<double>{0, 0, 0, 0, 0, 0}).ok);
  step_while(arm, cell::ARM_MOVING);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
  EXPECT_EQ(arm.position(0), 0.0);
}

TEST(ArmModel, EStopWhileIdleLatchesAndResetsImmediately) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.estop().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);
  EXPECT_TRUE(arm.reset().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
}

TEST(ArmModel, ResetWithoutEStopIsANoOp) {
  ArmModel arm(test_config());
  ASSERT_TRUE(arm.move_to(std::vector<double>{1.0, 0, 0, 0, 0, 0}).ok);
  arm.step(kDt);
  EXPECT_TRUE(arm.reset().ok);
  EXPECT_EQ(arm.mode(), cell::ARM_MOVING);
}

TEST(ArmModel, HandleDispatchesCommands) {
  ArmModel arm(test_config());

  cell::Command move;
  for (const double j : {0.1, 0.2, 0.3, 0.4, 0.5, 0.6}) {
    move.mutable_move_to()->add_joints(j);
  }
  EXPECT_TRUE(arm.handle(move).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_MOVING);

  cell::Command stop;
  stop.mutable_stop();
  EXPECT_TRUE(arm.handle(stop).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_STOPPING);

  cell::Command estop;
  estop.mutable_estop();
  EXPECT_TRUE(arm.handle(estop).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_ESTOP);

  cell::Command reset;
  reset.mutable_reset();
  while (!arm.at_rest()) {
    arm.step(kDt);
  }
  EXPECT_TRUE(arm.handle(reset).ok);
  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
}

TEST(ArmModel, HandleRejectsUnsupportedActions) {
  ArmModel arm(test_config());

  cell::Command speed;
  speed.mutable_set_conveyor_speed()->set_speed(0.5F);
  auto r = arm.handle(speed);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("set_conveyor_speed"), std::string::npos) << r.error;

  cell::Command fault;
  fault.mutable_fault_inject()->set_kind(cell::FaultInject::DISCONNECT);
  r = arm.handle(fault);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("fault_inject"), std::string::npos) << r.error;

  r = arm.handle(cell::Command{});
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("no action"), std::string::npos) << r.error;

  EXPECT_EQ(arm.mode(), cell::ARM_IDLE);
}

TEST(ArmModel, FillWritesJointState) {
  ArmConfig c = test_config();
  c.home = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6};
  ArmModel arm(c);
  cell::JointState state;
  arm.fill(state);
  ASSERT_EQ(state.position_size(), 6);
  ASSERT_EQ(state.velocity_size(), 6);
  EXPECT_DOUBLE_EQ(state.position(2), 0.3);
  EXPECT_EQ(state.mode(), cell::ARM_IDLE);

  ASSERT_TRUE(arm.move_to(std::vector<double>{1, 1, 1, 1, 1, 1}).ok);
  arm.step(kDt);
  arm.fill(state);  // refilling a reused message must not grow it
  ASSERT_EQ(state.position_size(), 6);
  EXPECT_EQ(state.mode(), cell::ARM_MOVING);
  EXPECT_GT(state.velocity(0), 0.0);
}

TEST(ArmConfig, FromJsonReadsKeysAndDefaultsHome) {
  const cell::Json j = {{"joints", 4},
                        {"joint_limit_rad", 2.0},
                        {"max_velocity", 1.0},
                        {"max_acceleration", 2.0},
                        {"estop_deceleration", 10.0}};
  const ArmConfig c = ArmConfig::from_json(j);
  EXPECT_EQ(c.joints, 4U);
  EXPECT_EQ(c.home, std::vector<double>(4, 0.0));
  EXPECT_DOUBLE_EQ(c.estop_deceleration, 10.0);
}

TEST(ArmConfig, RejectsInvalidValues) {
  const cell::Json good = {
      {"joints", 2},         {"home", {0.0, 0.0}},      {"joint_limit_rad", 2.0},
      {"max_velocity", 1.0}, {"max_acceleration", 2.0}, {"estop_deceleration", 10.0}};
  EXPECT_NO_THROW(ArmConfig::from_json(good));

  cell::Json bad = good;
  bad["home"] = {0.0};  // wrong length
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
  bad = good;
  bad["home"] = {0.0, 2.5};  // outside the joint limit
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
  bad = good;
  bad["max_velocity"] = 0.0;
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
  bad = good;
  bad["max_acceleration"] = -1.0;
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
  bad = good;
  bad["joints"] = 0;
  bad["home"] = cell::Json::array();
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
  bad = good;
  bad.erase("estop_deceleration");
  EXPECT_THROW(ArmConfig::from_json(bad), cell::ConfigError);
}

}  // namespace
