// Broker processing thread: consumer side of the ring. Owns the consumer-facing
// XPUB socket and everything that is allowed to take time - statistics, full
// heartbeat parsing, FrameInfo extraction, downsampling, CellState/CellStats.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <zmq.hpp>

#include "broker/downsampler.hpp"
#include "broker/frame_scan.hpp"
#include "broker/health_table.hpp"
#include "broker/shared.hpp"
#include "broker/stream_tracker.hpp"
#include "cell.pb.h"
#include "common/clock.hpp"
#include "common/heartbeat.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/zmq_util.hpp"

namespace cell::broker {
namespace {

constexpr std::uint64_t kNsPerSec = 1'000'000'000ULL;
constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
// Ring items handled before the timers and the ring high-water mark are looked
// at again; bounds how late a tick can be when a backlog is being worked off.
constexpr std::size_t kPopBatch = 256;

// What the broker does with a data stream beyond measuring it.
enum class Role { kStatsOnly, kArmJoint, kSensorFrame, kPlcConveyor };

Role role_for(const std::string& topic_name) {
  if (topic_name == topic::kArmJoint) return Role::kArmJoint;
  if (topic_name == topic::kSensorFrame) return Role::kSensorFrame;
  if (topic_name == topic::kPlcConveyor) return Role::kPlcConveyor;
  return Role::kStatsOnly;
}

zmq::socket_t make_xpub(zmq::context_t& ctx, const std::string& endpoint, int sndhwm) {
  zmq::socket_t sock(ctx, zmq::socket_type::xpub);
  sock.set(zmq::sockopt::linger, 0);
  sock.set(zmq::sockopt::sndhwm, sndhwm);
  ensure_ipc_dir(endpoint);
  sock.bind(endpoint);
  return sock;
}

// [topic][frame] with the frame's bytes forwarded as they are. Never blocks: a
// consumer that cannot keep up loses messages at its own HWM.
void send_raw(zmq::socket_t& sock, std::string_view topic_name, zmq::message_t& frame) {
  try {
    if (sock.send(zmq::buffer(topic_name), zmq::send_flags::sndmore | zmq::send_flags::dontwait)) {
      sock.send(frame, zmq::send_flags::dontwait);
    }
  } catch (const zmq::error_t& e) {
    if (e.num() != EINTR) {
      throw;
    }
  }
}

class Processor {
 public:
  explicit Processor(Shared& shared)
      : shared_(shared),
        cfg_(shared.cfg),
        xpub_(make_xpub(shared.ctx, cfg_.pub_endpoint, cfg_.pub_sndhwm)),
        start_ns_(mono_now_ns()),
        period_ns_(static_cast<std::uint64_t>(static_cast<double>(kNsPerSec) / cfg_.downsample_hz)),
        health_(cfg_.expected_processes(), cfg_.heartbeat_timeout_ms * kNsPerMs, start_ns_),
        heartbeat_(process::kBroker, cfg_.heartbeat_interval_ms),
        arm_(period_ns_),
        conveyor_(period_ns_),
        state_timer_(period_ns_, start_ns_),
        stats_timer_(cfg_.stats_interval_ms * kNsPerMs, start_ns_),
        window_start_ns_(start_ns_) {
    for (const StreamConfig& stream : cfg_.streams) {
      // Heartbeat-only entries get no tracker and no StreamStats row.
      trackers_.push_back(stream.has_data() ? std::optional<StreamTracker>(stream.topic)
                                            : std::nullopt);
      roles_.push_back(role_for(stream.topic));
    }
    heartbeat_.set_status(PROCESS_OK);
    spdlog::info("publishing to consumers on {} (downsample {} Hz, stats every {} ms)",
                 cfg_.pub_endpoint, cfg_.downsample_hz, cfg_.stats_interval_ms);
  }

  void run() {
    const std::chrono::microseconds idle_sleep(cfg_.idle_sleep_us);
    RingItem item;
    while (!shutdown_requested()) {
      shared_.processing.beats.fetch_add(1, std::memory_order_relaxed);

      // Occupancy only grows while this thread is not popping, so sampling it
      // right before each batch catches the peaks.
      ring_high_water_ = std::max<std::uint64_t>(ring_high_water_, shared_.ring.size_approx());
      std::size_t popped = 0;
      while (popped < kPopBatch && shared_.ring.try_pop(item)) {
        ++popped;
        handle(item);
      }

      const std::uint64_t now = mono_now_ns();
      publish_downsampled(now);
      if (state_timer_.due(now)) {
        publish_state(now);
        heartbeat_.tick(xpub_, now);
        drain_subscriptions();
      }
      if (stats_timer_.due(now)) {
        publish_stats(now);
      }

      if (popped < kPopBatch) {
        // Ring empty. Waiting strategy: a plain short sleep.
        //  * Spinning or yield() would hold a core at 100 % for a 1 kHz load.
        //  * Blocking on a condvar/eventfd would need the I/O thread to make a
        //    wake-up syscall per message - cost on the one path that must stay
        //    cheap, and it couples the two threads through a lock/kernel
        //    object, which is exactly what the lock-free ring avoids.
        //  * Latency measurements are stamped on the I/O thread, so sleeping
        //    here adds nothing to them; it only delays consumer output by at
        //    most idle_sleep_us (0.5 ms by default against a 20 ms downsample
        //    period), and at the default rates the ring never holds more than
        //    a couple of items when this thread wakes.
        std::this_thread::sleep_for(idle_sleep);
      }
    }
    heartbeat_.set_status(PROCESS_STOPPING);
    heartbeat_.publish_now(xpub_, mono_now_ns());
    log_totals();
  }

 private:
  void handle(RingItem& item) {
    if (item.kind == ItemKind::kHeartbeat) {
      handle_heartbeat(item);
      return;
    }
    StreamTracker& tracker = *trackers_[item.stream];
    const auto event =
        tracker.on_message(item.seq, latency_ns(item.send_ns, item.recv_ns), item.size);
    if (event == StreamTracker::SeqEvent::kRestart) {
      spdlog::warn("{}: sequence went back to {} - publisher restarted (restart #{}, not a gap)",
                   tracker.name(), item.seq, tracker.restarts());
    } else if (event == StreamTracker::SeqEvent::kGap) {
      spdlog::debug("{}: sequence gap before seq {}", tracker.name(), item.seq);
    }

    switch (roles_[item.stream]) {
      case Role::kArmJoint:
        arm_.push(std::move(item.payload));
        break;
      case Role::kPlcConveyor:
        conveyor_.push(std::move(item.payload));
        break;
      case Role::kSensorFrame:
        publish_frame_info(item);
        break;
      case Role::kStatsOnly:
        break;
    }
    // Release the buffer now rather than when the slot is next overwritten; for
    // a point cloud that is megabytes.
    item.payload = zmq::message_t();
  }

  void handle_heartbeat(const RingItem& item) {
    if (!parse_proto(item.payload, hb_scratch_)) {
      ++bad_messages_;
      return;
    }
    const auto event =
        health_.on_heartbeat(hb_scratch_.h().source(), hb_scratch_.status(),
                             hb_scratch_.uptime_ms(), hb_scratch_.detail(), item.recv_ns);
    switch (event) {
      case HealthTable::Event::kFirst:
        spdlog::info("first heartbeat from {}", hb_scratch_.h().source());
        break;
      case HealthTable::Event::kRestarted:
        spdlog::warn("{} restarted (uptime went back to {} ms)", hb_scratch_.h().source(),
                     hb_scratch_.uptime_ms());
        break;
      case HealthTable::Event::kUnknownSource:
        if (++unknown_heartbeats_ == 1) {
          spdlog::warn("ignoring heartbeats from unexpected process '{}'",
                       hb_scratch_.h().source());
        }
        break;
      case HealthTable::Event::kUpdated:
        break;
    }
  }

  // Consumers on tcp get the frame's metadata, never the cloud itself.
  void publish_frame_info(const RingItem& item) {
    FrameInfo& info = *state_.mutable_sensor();
    if (!scan_frame_info(item.payload.data(), item.payload.size(), info)) {
      state_.clear_sensor();
      ++bad_messages_;
      return;
    }
    send_proto(xpub_, topic::kSensorFrame, info);
  }

  // Forward the newest JointState / ConveyorState at most once per period. The
  // original bytes go out untouched, so the publisher's header (seq and send
  // timestamp) reaches consumers and they can compute end-to-end latency.
  void publish_downsampled(std::uint64_t now) {
    if (auto msg = arm_.poll(now)) {
      if (parse_proto(*msg, *state_.mutable_arm())) {
        send_raw(xpub_, topic::kArmJoint, *msg);
      } else {
        state_.clear_arm();
        ++bad_messages_;
      }
    }
    if (auto msg = conveyor_.poll(now)) {
      if (parse_proto(*msg, *state_.mutable_conveyor())) {
        send_raw(xpub_, topic::kPlcConveyor, *msg);
      } else {
        state_.clear_conveyor();
        ++bad_messages_;
      }
    }
  }

  void publish_state(std::uint64_t now) {
    state_.clear_processes();
    bool healthy = health_.fill(now, *state_.mutable_processes());
    // The broker reports itself: if this message exists, this thread is alive.
    ProcessHealth& self = *state_.add_processes();
    self.set_name(process::kBroker);
    self.set_status(heartbeat_.status());
    self.set_uptime_ms((now - start_ns_) / kNsPerMs);
    self.set_last_seen_age_ms(0);
    self.set_alive(true);
    healthy = healthy && heartbeat_.status() == PROCESS_OK;
    state_.set_healthy(healthy);
    if (healthy != last_healthy_) {
      spdlog::log(healthy ? spdlog::level::info : spdlog::level::warn, "cell is {}",
                  healthy ? "healthy" : "NOT healthy");
      last_healthy_ = healthy;
    }
    fill_header(*state_.mutable_h(), state_seq_++, process::kBroker);
    send_proto(xpub_, topic::kCellState, state_);
  }

  void publish_stats(std::uint64_t now) {
    const std::uint64_t elapsed = now - window_start_ns_;
    window_start_ns_ = now;
    stats_.clear_streams();
    for (std::size_t i = 0; i < trackers_.size(); ++i) {
      if (trackers_[i].has_value()) {
        trackers_[i]->snapshot(elapsed, shared_.drops[i].get(), *stats_.add_streams());
      }
    }
    stats_.set_window_ms((elapsed + kNsPerMs / 2) / kNsPerMs);
    stats_.set_ring_capacity(shared_.ring.capacity());
    stats_.set_ring_high_water(ring_high_water_);
    ring_high_water_ = 0;
    fill_header(*stats_.mutable_h(), stats_seq_++, process::kBroker);
    send_proto(xpub_, topic::kCellStats, stats_);

    for (const StreamStats& s : stats_.streams()) {
      if (s.seq_gaps() != 0 || s.drops() != 0) {
        spdlog::warn("{}: {} sequence gaps, {} ring drops in the last {} ms", s.stream(),
                     s.seq_gaps(), s.drops(), stats_.window_ms());
      }
      spdlog::debug("{}: {:.1f} Hz p50 {} ns p99 {} ns p99.9 {} ns max {} ns", s.stream(),
                    s.rate_hz(), s.p50_ns(), s.p99_ns(), s.p999_ns(), s.max_ns());
    }
  }

  // XPUB delivers every subscribe/unsubscribe as an inbound message. They must
  // be read, or the socket's inbound queue fills and the wake-ups never stop.
  void drain_subscriptions() {
    zmq::message_t frame;
    try {
      while (xpub_.recv(frame, zmq::recv_flags::dontwait)) {
        if (frame.size() >= 1 && spdlog::should_log(spdlog::level::debug)) {
          const auto* bytes = static_cast<const char*>(frame.data());
          spdlog::debug("consumer {} '{}'", bytes[0] != 0 ? "subscribed to" : "unsubscribed from",
                        std::string_view(bytes + 1, frame.size() - 1));
        }
      }
    } catch (const zmq::error_t& e) {
      if (e.num() != EINTR) {
        throw;
      }
    }
  }

  void log_totals() const {
    for (std::size_t i = 0; i < trackers_.size(); ++i) {
      if (trackers_[i].has_value()) {
        const StreamTracker& t = *trackers_[i];
        spdlog::info("{}: {} messages, {} sequence gaps, {} ring drops, {} publisher restarts",
                     t.name(), t.total_count(), t.total_seq_gaps(), shared_.drops[i].get(),
                     t.restarts());
      }
    }
    spdlog::info("republished {} of {} joint states, {} of {} conveyor states", arm_.emitted(),
                 arm_.received(), conveyor_.emitted(), conveyor_.received());
    const std::uint64_t bad = bad_messages_ + shared_.malformed.get();
    if (bad != 0 || shared_.hb_drops.get() != 0 || unknown_heartbeats_ != 0) {
      spdlog::warn(
          "{} malformed messages, {} heartbeats dropped at the ring, {} heartbeats from "
          "unexpected processes",
          bad, shared_.hb_drops.get(), unknown_heartbeats_);
    }
  }

  Shared& shared_;
  const BrokerConfig& cfg_;
  zmq::socket_t xpub_;
  std::uint64_t start_ns_;
  std::uint64_t period_ns_;

  std::vector<std::optional<StreamTracker>> trackers_;  // index = stream index
  std::vector<Role> roles_;
  HealthTable health_;
  HeartbeatEmitter heartbeat_;
  Downsampler<zmq::message_t> arm_;
  Downsampler<zmq::message_t> conveyor_;
  PeriodicTimer state_timer_;
  PeriodicTimer stats_timer_;

  // Reused across publishes; `state_` also holds the latest arm / conveyor /
  // sensor snapshot between ticks.
  CellState state_;
  CellStats stats_;
  Heartbeat hb_scratch_;
  std::uint64_t state_seq_ = 0;
  std::uint64_t stats_seq_ = 0;
  std::uint64_t window_start_ns_;
  std::uint64_t ring_high_water_ = 0;
  std::uint64_t bad_messages_ = 0;
  std::uint64_t unknown_heartbeats_ = 0;
  bool last_healthy_ = false;
};

}  // namespace

void run_processing_thread(Shared& shared) {
  Processor processor(shared);
  shared.processing.ready.store(true);
  processor.run();
}

}  // namespace cell::broker
