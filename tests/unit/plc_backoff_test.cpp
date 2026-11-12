#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <vector>

#include "common/config.hpp"
#include "plc_bridge/backoff.hpp"
#include "plc_bridge/conveyor_map.hpp"

namespace {

using cell::plc::Backoff;
using cell::plc::BackoffConfig;
using cell::plc::ReconnectSchedule;
using cell::plc::RegisterMap;

constexpr std::uint64_t kMs = 1'000'000ULL;

BackoffConfig config(double initial, double max, double multiplier, double jitter) {
  BackoffConfig c;
  c.initial_ms = initial;
  c.max_ms = max;
  c.multiplier = multiplier;
  c.jitter = jitter;
  return c;
}

TEST(PlcBackoff, GrowsExponentiallyWithoutJitter) {
  Backoff b(config(100, 5000, 2.0, 0.0), 1);
  const std::vector<std::uint64_t> expected{100, 200, 400, 800, 1600, 3200, 5000, 5000, 5000};
  for (const std::uint64_t want : expected) {
    EXPECT_EQ(b.next_delay_ms(), want);
  }
}

TEST(PlcBackoff, CapsAtMax) {
  Backoff b(config(100, 450, 3.0, 0.0), 1);
  EXPECT_EQ(b.next_delay_ms(), 100U);
  EXPECT_EQ(b.next_delay_ms(), 300U);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(b.next_delay_ms(), 450U);
  }
  EXPECT_DOUBLE_EQ(b.base_ms(), 450.0);  // the base itself saturates, no overflow
}

TEST(PlcBackoff, ResetReturnsToInitial) {
  Backoff b(config(100, 5000, 2.0, 0.0), 1);
  for (int i = 0; i < 5; ++i) {
    b.next_delay_ms();
  }
  EXPECT_GT(b.base_ms(), 100.0);
  b.reset();
  EXPECT_DOUBLE_EQ(b.base_ms(), 100.0);
  EXPECT_EQ(b.next_delay_ms(), 100U);
  EXPECT_EQ(b.next_delay_ms(), 200U);
}

TEST(PlcBackoff, JitterStaysWithinBounds) {
  const double jitter = 0.2;
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    Backoff b(config(100, 5000, 2.0, jitter), seed);
    double base = 100.0;
    for (int i = 0; i < 12; ++i) {
      const auto delay = static_cast<double>(b.next_delay_ms());
      EXPECT_GE(delay, base * (1.0 - jitter) - 0.5) << "seed " << seed << " step " << i;
      EXPECT_LE(delay, base * (1.0 + jitter) + 0.5) << "seed " << seed << " step " << i;
      EXPECT_LE(delay, 5000.0);  // jitter never pushes past the cap
      base = std::min(base * 2.0, 5000.0);
    }
  }
}

TEST(PlcBackoff, JitterActuallyVariesAndIsSeedDeterministic) {
  std::set<std::uint64_t> first_delays;
  for (std::uint64_t seed = 0; seed < 50; ++seed) {
    Backoff b(config(1000, 60'000, 2.0, 0.2), seed);
    first_delays.insert(b.next_delay_ms());
  }
  EXPECT_GT(first_delays.size(), 25U);  // spread over [800, 1200], not one value

  Backoff a(config(100, 5000, 2.0, 0.2), 1234);
  Backoff b(config(100, 5000, 2.0, 0.2), 1234);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(a.next_delay_ms(), b.next_delay_ms());
  }
}

TEST(PlcBackoff, JitterUsesBothSides) {
  Backoff b(config(1000, 1000, 1.0, 0.5), 99);  // constant base: only jitter varies
  bool below = false;
  bool above = false;
  for (int i = 0; i < 200; ++i) {
    const std::uint64_t d = b.next_delay_ms();
    ASSERT_GE(d, 500U);
    ASSERT_LE(d, 1000U);  // capped at max_ms
    below = below || d < 900;
    above = above || d > 900;
  }
  EXPECT_TRUE(below);
  EXPECT_TRUE(above);
}

TEST(PlcBackoff, DelayIsNeverZero) {
  Backoff b(config(1, 1, 1.0, 0.99), 5);
  for (int i = 0; i < 100; ++i) {
    EXPECT_GE(b.next_delay_ms(), 1U);
  }
}

TEST(PlcBackoff, ConfigValidation) {
  EXPECT_NO_THROW(config(100, 5000, 2.0, 0.2).validate());
  EXPECT_NO_THROW(config(100, 100, 1.0, 0.0).validate());
  EXPECT_THROW(config(0, 5000, 2.0, 0.2).validate(), cell::ConfigError);
  EXPECT_THROW(config(100, 50, 2.0, 0.2).validate(), cell::ConfigError);
  EXPECT_THROW(config(100, 5000, 0.5, 0.2).validate(), cell::ConfigError);
  EXPECT_THROW(config(100, 5000, 2.0, -0.1).validate(), cell::ConfigError);
  EXPECT_THROW(config(100, 5000, 2.0, 1.0).validate(), cell::ConfigError);
  EXPECT_THROW(Backoff(config(100, 50, 2.0, 0.2), 1), cell::ConfigError);
}

TEST(PlcBackoff, ConfigFromJson) {
  const cell::Json j = {
      {"initial_ms", 100}, {"max_ms", 5000}, {"multiplier", 2.0}, {"jitter", 0.2}};
  const BackoffConfig c = BackoffConfig::from_json(j);
  EXPECT_DOUBLE_EQ(c.initial_ms, 100.0);
  EXPECT_DOUBLE_EQ(c.max_ms, 5000.0);
  EXPECT_DOUBLE_EQ(c.multiplier, 2.0);
  EXPECT_DOUBLE_EQ(c.jitter, 0.2);

  cell::Json missing = j;
  missing.erase("max_ms");
  EXPECT_THROW(BackoffConfig::from_json(missing), cell::ConfigError);
}

TEST(PlcReconnectSchedule, FirstAttemptIsImmediate) {
  ReconnectSchedule s(Backoff(config(100, 5000, 2.0, 0.0), 1));
  EXPECT_TRUE(s.due(0));
  EXPECT_TRUE(s.due(12345));
}

TEST(PlcReconnectSchedule, WaitsOutGrowingDelaysBetweenFailures) {
  ReconnectSchedule s(Backoff(config(100, 5000, 2.0, 0.0), 1));
  std::uint64_t now = 1000 * kMs;

  EXPECT_EQ(s.on_failure(now), 100U);
  EXPECT_FALSE(s.due(now));
  EXPECT_FALSE(s.due(now + 99 * kMs));
  EXPECT_TRUE(s.due(now + 100 * kMs));

  now += 100 * kMs;
  EXPECT_EQ(s.on_failure(now), 200U);
  EXPECT_FALSE(s.due(now + 199 * kMs));
  EXPECT_TRUE(s.due(now + 200 * kMs));

  now += 200 * kMs;
  EXPECT_EQ(s.on_failure(now), 400U);
}

TEST(PlcReconnectSchedule, SuccessResetsBackoff) {
  ReconnectSchedule s(Backoff(config(100, 5000, 2.0, 0.0), 1));
  std::uint64_t now = 0;
  for (int i = 0; i < 6; ++i) {
    now += s.on_failure(now) * kMs;
  }
  s.on_connected();
  EXPECT_TRUE(s.due(now));
  // The next outage starts again from the initial delay.
  EXPECT_EQ(s.on_failure(now), 100U);
}

// ---- register map / command conversions ------------------------------------

TEST(PlcConveyorMap, ValidateSpeed) {
  EXPECT_FALSE(cell::plc::validate_speed(0.0, 2.0).has_value());
  EXPECT_FALSE(cell::plc::validate_speed(2.0, 2.0).has_value());
  EXPECT_FALSE(cell::plc::validate_speed(0.75, 2.0).has_value());
  EXPECT_TRUE(cell::plc::validate_speed(-0.01, 2.0).has_value());
  EXPECT_TRUE(cell::plc::validate_speed(2.01, 2.0).has_value());
  EXPECT_TRUE(cell::plc::validate_speed(std::nan(""), 2.0).has_value());
  EXPECT_TRUE(cell::plc::validate_speed(HUGE_VAL, 2.0).has_value());
  EXPECT_NE(cell::plc::validate_speed(3.0, 2.0)->find("outside"), std::string::npos);
}

TEST(PlcConveyorMap, SpeedFixedPointRoundTrip) {
  EXPECT_EQ(cell::plc::speed_to_register(0.0, 1000.0), 0U);
  EXPECT_EQ(cell::plc::speed_to_register(0.5, 1000.0), 500U);
  EXPECT_EQ(cell::plc::speed_to_register(1.2344, 1000.0), 1234U);
  EXPECT_EQ(cell::plc::speed_to_register(1.2346, 1000.0), 1235U);
  EXPECT_EQ(cell::plc::speed_to_register(2.0, 1000.0), 2000U);
  // Saturates rather than wrapping.
  EXPECT_EQ(cell::plc::speed_to_register(100.0, 1000.0), 65535U);
  EXPECT_EQ(cell::plc::speed_to_register(-1.0, 1000.0), 0U);
  EXPECT_EQ(cell::plc::speed_to_register(std::nan(""), 1000.0), 0U);

  EXPECT_FLOAT_EQ(cell::plc::register_to_speed(500, 1000.0), 0.5F);
  EXPECT_FLOAT_EQ(cell::plc::register_to_speed(0, 1000.0), 0.0F);
  EXPECT_FLOAT_EQ(cell::plc::register_to_speed(65535, 100.0), 655.35F);
  for (const double speed : {0.0, 0.001, 0.5, 1.337, 2.0}) {
    EXPECT_NEAR(cell::plc::register_to_speed(cell::plc::speed_to_register(speed, 1000.0), 1000.0),
                speed, 0.0005);
  }
}

TEST(PlcConveyorMap, OutageRegisterValue) {
  EXPECT_EQ(cell::plc::outage_register_value(0), 65535U);  // until restart
  EXPECT_EQ(cell::plc::outage_register_value(1), 1U);
  EXPECT_EQ(cell::plc::outage_register_value(1500), 1500U);
  EXPECT_EQ(cell::plc::outage_register_value(65534), 65534U);
  // A long finite outage is clamped below the "until restart" sentinel.
  EXPECT_EQ(cell::plc::outage_register_value(65535), 65534U);
  EXPECT_EQ(cell::plc::outage_register_value(10'000'000), 65534U);
  EXPECT_EQ(cell::plc::outage_register_value(0xFFFFFFFFU), 65534U);
}

TEST(PlcConveyorMap, RegisterMapFromJson) {
  const cell::Json j = {{"speed_scale", 1000},  {"ir_speed_actual", 0},   {"ir_fault_code", 1},
                        {"di_item_present", 0}, {"hr_speed_setpoint", 0}, {"hr_sim_outage_ms", 10}};
  const RegisterMap m = RegisterMap::from_json(j);
  EXPECT_DOUBLE_EQ(m.speed_scale, 1000.0);
  EXPECT_EQ(m.ir_speed_actual, 0);
  EXPECT_EQ(m.ir_fault_code, 1);
  EXPECT_EQ(m.di_item_present, 0);
  EXPECT_EQ(m.hr_speed_setpoint, 0);
  EXPECT_EQ(m.hr_sim_outage_ms, 10);

  cell::Json bad = j;
  bad["speed_scale"] = 0;
  EXPECT_THROW(RegisterMap::from_json(bad), cell::ConfigError);
  bad = j;
  bad["ir_fault_code"] = -1;
  EXPECT_THROW(RegisterMap::from_json(bad), cell::ConfigError);
  bad = j;
  bad["hr_sim_outage_ms"] = 70000;
  EXPECT_THROW(RegisterMap::from_json(bad), cell::ConfigError);
  bad = j;
  bad.erase("di_item_present");
  EXPECT_THROW(RegisterMap::from_json(bad), cell::ConfigError);
}

}  // namespace
