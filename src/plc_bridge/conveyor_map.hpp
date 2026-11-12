#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "common/config.hpp"

namespace cell::plc {

// Modbus register map shared with plc_sim (the "registers" config block).
// Speeds are fixed-point: register = m/s * speed_scale.
struct RegisterMap {
  double speed_scale = 1000.0;
  int ir_speed_actual = 0;    // input register
  int ir_fault_code = 1;      // input register
  int di_item_present = 0;    // discrete input
  int hr_speed_setpoint = 0;  // holding register
  int hr_sim_outage_ms = 10;  // holding register (fault injection)

  // Throws ConfigError on a missing key, a negative/too-large address or a
  // non-positive scale.
  static RegisterMap from_json(const Json& block);
};

// plc_sim reads this value of hr_sim_outage_ms as "stay down until restarted".
inline constexpr std::uint16_t kOutageUntilRestart = 65535;

// Error text if `speed` (m/s) is not a valid setpoint, nullopt if it is.
std::optional<std::string> validate_speed(double speed, double max_speed);

// m/s -> register value (rounded, saturating at the uint16 range).
std::uint16_t speed_to_register(double speed, double speed_scale) noexcept;

// Register value -> m/s.
float register_to_speed(std::uint16_t value, double speed_scale) noexcept;

// FaultInject.duration_ms -> value for hr_sim_outage_ms. 0 means "until
// restart" (65535). Timed outages are clamped to 1..65534 ms so that a long
// but finite request can never turn into the permanent one.
std::uint16_t outage_register_value(std::uint32_t duration_ms) noexcept;

}  // namespace cell::plc
