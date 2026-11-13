#include "broker/stream_tracker.hpp"

#include <utility>

namespace cell::broker {

StreamTracker::StreamTracker(std::string name) : name_(std::move(name)) {}

StreamTracker::SeqEvent StreamTracker::on_message(std::uint64_t seq, std::uint64_t latency_ns,
                                                  std::uint64_t bytes) noexcept {
  window_latency_.record(latency_ns);
  window_bytes_ += bytes;
  total_bytes_ += bytes;
  ++total_count_;

  SeqEvent event = SeqEvent::kInOrder;
  if (!synced_) {
    synced_ = true;
    event = SeqEvent::kFirst;
  } else if (seq > expected_seq_) {
    const std::uint64_t missing = seq - expected_seq_;
    window_gaps_ += missing;
    total_gaps_ += missing;
    event = SeqEvent::kGap;
  } else if (seq < expected_seq_) {
    ++restarts_;
    event = SeqEvent::kRestart;
  }
  expected_seq_ = seq + 1;
  return event;
}

void StreamTracker::snapshot(std::uint64_t window_ns, std::uint64_t total_drops, StreamStats& out) {
  const std::uint64_t count = window_latency_.count();
  // A counter that went backwards would be a bug upstream; report 0, not 2^64.
  const std::uint64_t drops = total_drops > reported_drops_ ? total_drops - reported_drops_ : 0;
  const double rate =
      window_ns == 0 ? 0.0 : static_cast<double>(count) * 1e9 / static_cast<double>(window_ns);
  out.set_stream(name_);
  out.set_count(count);
  out.set_rate_hz(rate);
  out.set_seq_gaps(window_gaps_);
  out.set_drops(drops);
  // percentile() and max() are 0 for an empty window.
  out.set_p50_ns(window_latency_.percentile(50.0));
  out.set_p99_ns(window_latency_.percentile(99.0));
  out.set_p999_ns(window_latency_.percentile(99.9));
  out.set_max_ns(window_latency_.max());
  out.set_total_count(total_count_);
  out.set_total_seq_gaps(total_gaps_);
  out.set_total_drops(total_drops);
  out.set_bytes(window_bytes_);

  window_latency_.reset();
  window_gaps_ = 0;
  window_bytes_ = 0;
  reported_drops_ = total_drops;
}

}  // namespace cell::broker
