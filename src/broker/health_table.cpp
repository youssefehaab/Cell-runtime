#include "broker/health_table.hpp"

#include <algorithm>

namespace cell::broker {
namespace {
constexpr std::uint64_t kNsPerMs = 1'000'000ULL;

// A heartbeat stamped after `now` (received between the caller reading the
// clock and draining the ring) has age 0, not a wrapped huge value.
std::uint64_t age_ns(std::uint64_t now_ns, std::uint64_t then_ns) noexcept {
  return now_ns > then_ns ? now_ns - then_ns : 0;
}
}  // namespace

HealthTable::HealthTable(const std::vector<std::string>& expected, std::uint64_t timeout_ns,
                         std::uint64_t start_ns)
    : timeout_ns_(timeout_ns), start_ns_(start_ns) {
  for (const auto& name : expected) {
    if (find(name) == nullptr) {
      Entry entry;
      entry.name = name;
      entries_.push_back(std::move(entry));
    }
  }
}

const HealthTable::Entry* HealthTable::find(std::string_view name) const noexcept {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& e) { return e.name == name; });
  return it == entries_.end() ? nullptr : &*it;
}

HealthTable::Event HealthTable::on_heartbeat(std::string_view source, ProcessStatus status,
                                             std::uint64_t uptime_ms, std::string_view detail,
                                             std::uint64_t recv_ns) {
  const auto it = std::find_if(entries_.begin(), entries_.end(),
                               [&](const Entry& e) { return e.name == source; });
  if (it == entries_.end()) {
    return Event::kUnknownSource;
  }
  Event event = Event::kUpdated;
  if (!it->seen) {
    event = Event::kFirst;
  } else if (uptime_ms < it->uptime_ms) {
    event = Event::kRestarted;
  }
  it->seen = true;
  it->status = status;
  it->uptime_ms = uptime_ms;
  it->last_seen_ns = recv_ns;
  it->detail.assign(detail);
  return event;
}

bool HealthTable::alive(const Entry& entry, std::uint64_t now_ns) const noexcept {
  return entry.seen && age_ns(now_ns, entry.last_seen_ns) <= timeout_ns_;
}

bool HealthTable::alive(std::string_view name, std::uint64_t now_ns) const noexcept {
  const Entry* entry = find(name);
  return entry != nullptr && alive(*entry, now_ns);
}

bool HealthTable::fill(std::uint64_t now_ns,
                       google::protobuf::RepeatedPtrField<ProcessHealth>& out) const {
  bool healthy = true;
  for (const Entry& entry : entries_) {
    const bool is_alive = alive(entry, now_ns);
    const std::uint64_t since = entry.seen ? entry.last_seen_ns : start_ns_;
    ProcessHealth& health = *out.Add();
    health.set_name(entry.name);
    health.set_status(entry.status);
    health.set_uptime_ms(entry.uptime_ms);
    health.set_last_seen_age_ms(age_ns(now_ns, since) / kNsPerMs);
    health.set_alive(is_alive);
    health.set_detail(entry.detail);
    healthy = healthy && is_alive && entry.status == PROCESS_OK;
  }
  return healthy;
}

}  // namespace cell::broker
