#pragma once

#include <cstdint>
#include <string>
#include <zmq.hpp>

#include "cell.pb.h"

namespace cell {

// Publishes Heartbeat messages on topic "hb" through a PUB socket owned by the
// caller. Not thread-safe: call tick() from the thread that owns the socket
// (zmq sockets must not be shared across threads).
class HeartbeatEmitter {
 public:
  HeartbeatEmitter(std::string source, std::uint64_t interval_ms);

  void set_status(ProcessStatus status, std::string detail = {});
  ProcessStatus status() const noexcept { return status_; }

  // Publish if the interval has elapsed. Returns true if a heartbeat was sent.
  bool tick(zmq::socket_t& pub, std::uint64_t now_mono_ns);

  // Publish immediately regardless of the interval (e.g. final STOPPING beat).
  void publish_now(zmq::socket_t& pub, std::uint64_t now_mono_ns);

 private:
  std::string source_;
  std::string detail_;
  std::uint64_t interval_ns_;
  std::uint64_t start_ns_;
  std::uint64_t last_ns_ = 0;
  std::uint64_t seq_ = 0;
  ProcessStatus status_ = PROCESS_STARTING;
};

}  // namespace cell
