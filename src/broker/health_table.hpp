#pragma once

#include <google/protobuf/repeated_field.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cell.pb.h"

namespace cell::broker {

// Last heartbeat of every expected process and the alive/healthy decision.
// Time is injected (CLOCK_MONOTONIC ns), so the timeout logic is unit-testable.
//
//   alive   = a heartbeat was received no longer than `timeout_ns` ago
//             (age == timeout still counts as alive).
//   healthy = every expected process is alive AND its last status is PROCESS_OK.
//
// A process that was never heard from is reported with alive = false, status
// UNSPECIFIED and last_seen_age_ms = time since the table was created, i.e.
// "not seen for at least this long".
class HealthTable {
 public:
  enum class Event {
    kUnknownSource,  // not an expected process: ignored
    kFirst,          // first heartbeat from this process
    kUpdated,
    kRestarted,  // uptime went backwards: the process was restarted
  };

  HealthTable(const std::vector<std::string>& expected, std::uint64_t timeout_ns,
              std::uint64_t start_ns);

  Event on_heartbeat(std::string_view source, ProcessStatus status, std::uint64_t uptime_ms,
                     std::string_view detail, std::uint64_t recv_ns);

  bool alive(std::string_view name, std::uint64_t now_ns) const noexcept;

  // Appends one ProcessHealth per expected process (construction order) and
  // returns the `healthy` verdict.
  bool fill(std::uint64_t now_ns, google::protobuf::RepeatedPtrField<ProcessHealth>& out) const;

  std::size_t size() const noexcept { return entries_.size(); }

 private:
  struct Entry {
    std::string name;
    bool seen = false;
    ProcessStatus status = PROCESS_STATUS_UNSPECIFIED;
    std::uint64_t uptime_ms = 0;
    std::uint64_t last_seen_ns = 0;
    std::string detail;
  };

  bool alive(const Entry& entry, std::uint64_t now_ns) const noexcept;
  const Entry* find(std::string_view name) const noexcept;

  std::vector<Entry> entries_;
  std::uint64_t timeout_ns_;
  std::uint64_t start_ns_;
};

}  // namespace cell::broker
