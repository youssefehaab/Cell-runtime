#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

// libmodbus is a C library; keep its header out of ours.
struct _modbus;

namespace cell::plc {

// RAII Modbus-TCP client connection (libmodbus). Every operation is bounded
// by the response timeout. Any failed read or write closes the connection:
// after an error the stream may be out of step with the server, and the
// caller's reaction is always "reconnect" anyway.
class ModbusLink {
 public:
  // `host` may be an IP address or a hostname. Throws std::runtime_error if
  // libmodbus cannot allocate a context.
  ModbusLink(const std::string& host, int port, int unit_id, std::uint32_t response_timeout_ms);
  ~ModbusLink();
  ModbusLink(const ModbusLink&) = delete;
  ModbusLink& operator=(const ModbusLink&) = delete;

  // Blocks for at most the response timeout. Returns false and sets
  // last_error() on failure.
  bool connect();
  void close() noexcept;
  bool connected() const noexcept { return connected_; }

  bool read_input_registers(int address, std::span<std::uint16_t> out);
  bool read_discrete_input(int address, bool& out);
  bool write_register(int address, std::uint16_t value);

  const std::string& last_error() const noexcept { return last_error_; }

 private:
  struct ContextDeleter {
    void operator()(_modbus* ctx) const noexcept;
  };

  bool fail(const char* what);

  std::unique_ptr<_modbus, ContextDeleter> ctx_;
  bool connected_ = false;
  std::string last_error_;
};

}  // namespace cell::plc
