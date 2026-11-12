#include "plc_bridge/conveyor_map.hpp"

#include <algorithm>
#include <cmath>

namespace cell::plc {
namespace {

int address(const Json& block, const char* key) {
  const int value = cfg_get<int>(block, key);
  if (value < 0 || value > 65535) {
    throw ConfigError(std::string("registers.") + key + " must be a Modbus address (0..65535)");
  }
  return value;
}

}  // namespace

RegisterMap RegisterMap::from_json(const Json& block) {
  RegisterMap m;
  m.speed_scale = cfg_get<double>(block, "speed_scale");
  if (!std::isfinite(m.speed_scale) || m.speed_scale <= 0.0) {
    throw ConfigError("registers.speed_scale must be positive");
  }
  m.ir_speed_actual = address(block, "ir_speed_actual");
  m.ir_fault_code = address(block, "ir_fault_code");
  m.di_item_present = address(block, "di_item_present");
  m.hr_speed_setpoint = address(block, "hr_speed_setpoint");
  m.hr_sim_outage_ms = address(block, "hr_sim_outage_ms");
  return m;
}

std::optional<std::string> validate_speed(double speed, double max_speed) {
  if (!std::isfinite(speed)) {
    return "conveyor speed is not a finite number";
  }
  if (speed < 0.0 || speed > max_speed) {
    return "conveyor speed " + std::to_string(speed) + " m/s is outside 0.." +
           std::to_string(max_speed) + " m/s";
  }
  return std::nullopt;
}

std::uint16_t speed_to_register(double speed, double speed_scale) noexcept {
  const double raw = std::round(speed * speed_scale);
  if (!(raw > 0.0)) {  // also catches NaN
    return 0;
  }
  return static_cast<std::uint16_t>(std::min(raw, 65535.0));
}

float register_to_speed(std::uint16_t value, double speed_scale) noexcept {
  return static_cast<float>(static_cast<double>(value) / speed_scale);
}

std::uint16_t outage_register_value(std::uint32_t duration_ms) noexcept {
  if (duration_ms == 0) {
    return kOutageUntilRestart;
  }
  return static_cast<std::uint16_t>(std::min<std::uint32_t>(duration_ms, kOutageUntilRestart - 1U));
}

}  // namespace cell::plc
