#include "plc_bridge/modbus_link.hpp"

#include <modbus.h>

#include <cerrno>
#include <stdexcept>

namespace cell::plc {

void ModbusLink::ContextDeleter::operator()(_modbus* ctx) const noexcept {
  modbus_close(ctx);
  modbus_free(ctx);
}

ModbusLink::ModbusLink(const std::string& host, int port, int unit_id,
                       std::uint32_t response_timeout_ms)
    // The "pi" (protocol-independent) variant resolves hostnames, which is
    // what a container deployment needs ("plc_sim" rather than an IP).
    : ctx_(modbus_new_tcp_pi(host.c_str(), std::to_string(port).c_str())) {
  if (!ctx_) {
    throw std::runtime_error(std::string("modbus_new_tcp_pi failed: ") + modbus_strerror(errno));
  }
  const std::uint32_t sec = response_timeout_ms / 1000;
  const std::uint32_t usec = (response_timeout_ms % 1000) * 1000;
  // The response timeout also bounds connect(); the byte timeout bounds gaps
  // inside one response. Together they cap how long a dead peer can stall us.
  if (modbus_set_slave(ctx_.get(), unit_id) != 0 ||
      modbus_set_response_timeout(ctx_.get(), sec, usec) != 0 ||
      modbus_set_byte_timeout(ctx_.get(), sec, usec) != 0) {
    throw std::runtime_error(std::string("invalid modbus settings: ") + modbus_strerror(errno));
  }
}

ModbusLink::~ModbusLink() = default;

bool ModbusLink::connect() {
  close();
  if (modbus_connect(ctx_.get()) != 0) {
    return fail("connect");
  }
  connected_ = true;
  last_error_.clear();
  return true;
}

void ModbusLink::close() noexcept {
  modbus_close(ctx_.get());
  connected_ = false;
}

bool ModbusLink::fail(const char* what) {
  last_error_ = std::string(what) + ": " + modbus_strerror(errno);
  close();
  return false;
}

bool ModbusLink::read_input_registers(int address, std::span<std::uint16_t> out) {
  if (!connected_) {
    last_error_ = "not connected";
    return false;
  }
  const int count = static_cast<int>(out.size());
  if (modbus_read_input_registers(ctx_.get(), address, count, out.data()) != count) {
    return fail("read input registers");
  }
  return true;
}

bool ModbusLink::read_discrete_input(int address, bool& out) {
  if (!connected_) {
    last_error_ = "not connected";
    return false;
  }
  std::uint8_t bit = 0;
  if (modbus_read_input_bits(ctx_.get(), address, 1, &bit) != 1) {
    return fail("read discrete input");
  }
  out = bit != 0;
  return true;
}

bool ModbusLink::write_register(int address, std::uint16_t value) {
  if (!connected_) {
    last_error_ = "not connected";
    return false;
  }
  if (modbus_write_register(ctx_.get(), address, value) != 1) {
    return fail("write register");
  }
  return true;
}

}  // namespace cell::plc
