#pragma once

#include <cstdint>
#include <string>

#include "cell.pb.h"
#include "common/histogram.hpp"

namespace cell::broker {

// Same-host latency: broker receive time minus the sender's stamp, both
// CLOCK_MONOTONIC. A sender stamp from the future (clock domains differ, or a
// corrupt header) is reported as 0 rather than wrapping to ~584 years.
constexpr std::uint64_t latency_ns(std::uint64_t send_ns, std::uint64_t recv_ns) noexcept {
  return recv_ns > send_ns ? recv_ns - send_ns : 0;
}

// Per-stream accounting on the processing thread: latency histogram, message
// and byte counts and sequence continuity, for the current stats window and
// since start. No clock and no sockets: time is passed in.
//
// Sequence rule (publishers number messages 0, 1, 2, ... per stream):
//   * first message seen  -> just synchronise; the broker may start late.
//   * seq == expected     -> in order.
//   * seq  > expected     -> (seq - expected) messages are missing: a gap.
//   * seq  < expected     -> the publisher restarted (or the transport
//                            reordered, which zmq does not do): count a
//                            restart, resynchronise, and count NO gap.
// A publisher that restarts and is first seen again at a seq at or above the
// old expected value is indistinguishable from a gap; at 1 kHz that needs a
// restart within milliseconds of the original start.
//
// seq_gaps vs drops: `drops` are messages the I/O thread discarded because the
// ring was full; they never reach this class, so each one also shows up here as
// a missing sequence number. seq_gaps therefore counts every message lost
// anywhere between the publisher and the processing thread (publisher HWM, SUB
// HWM, ring), and seq_gaps - drops approximates the upstream losses. The two
// are reported as measured and are deliberately not subtracted: a drop and the
// gap it causes may land in adjacent windows.
class StreamTracker {
 public:
  enum class SeqEvent { kFirst, kInOrder, kGap, kRestart };

  explicit StreamTracker(std::string name);

  SeqEvent on_message(std::uint64_t seq, std::uint64_t latency_ns, std::uint64_t bytes) noexcept;

  // Fill `out` with the window that ends now and start a new one.
  // `window_ns` is the real elapsed time (rate = count / elapsed), and
  // `total_drops` the ring-drop counter for this stream since start; the
  // window's drops are the increase since the previous snapshot.
  void snapshot(std::uint64_t window_ns, std::uint64_t total_drops, StreamStats& out);

  const std::string& name() const noexcept { return name_; }
  std::uint64_t total_count() const noexcept { return total_count_; }
  std::uint64_t total_seq_gaps() const noexcept { return total_gaps_; }
  std::uint64_t total_bytes() const noexcept { return total_bytes_; }
  std::uint64_t restarts() const noexcept { return restarts_; }

 private:
  std::string name_;
  LatencyHistogram window_latency_;
  bool synced_ = false;
  std::uint64_t expected_seq_ = 0;
  std::uint64_t window_gaps_ = 0;
  std::uint64_t window_bytes_ = 0;
  std::uint64_t total_count_ = 0;
  std::uint64_t total_gaps_ = 0;
  std::uint64_t total_bytes_ = 0;
  std::uint64_t reported_drops_ = 0;
  std::uint64_t restarts_ = 0;
};

}  // namespace cell::broker
