#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "broker/command_queue.hpp"
#include "broker/downsampler.hpp"
#include "broker/frame_scan.hpp"
#include "broker/health_table.hpp"
#include "broker/liveness.hpp"
#include "broker/stream_tracker.hpp"
#include "common/zmq_util.hpp"

namespace cell::broker {
namespace {

constexpr std::uint64_t kMs = 1'000'000;
constexpr std::uint64_t kSec = 1'000'000'000;

// ---------------------------------------------------------------- latency ---

TEST(BrokerLatency, ClampsNegativeToZero) {
  EXPECT_EQ(latency_ns(100, 350), 250U);
  EXPECT_EQ(latency_ns(350, 100), 0U);
  EXPECT_EQ(latency_ns(7, 7), 0U);
}

// ---------------------------------------------------------- StreamTracker ---

TEST(BrokerStreamTracker, FirstMessageSynchronisesWithoutGap) {
  StreamTracker t("arm.joint");
  EXPECT_EQ(t.on_message(500, 1000, 10), StreamTracker::SeqEvent::kFirst);
  EXPECT_EQ(t.on_message(501, 1000, 10), StreamTracker::SeqEvent::kInOrder);
  EXPECT_EQ(t.total_seq_gaps(), 0U);
  EXPECT_EQ(t.total_count(), 2U);
}

TEST(BrokerStreamTracker, CountsEveryMissingSequenceNumber) {
  StreamTracker t("arm.joint");
  t.on_message(0, 1, 1);
  EXPECT_EQ(t.on_message(4, 1, 1), StreamTracker::SeqEvent::kGap);  // 1, 2, 3 missing
  EXPECT_EQ(t.on_message(5, 1, 1), StreamTracker::SeqEvent::kInOrder);
  EXPECT_EQ(t.on_message(7, 1, 1), StreamTracker::SeqEvent::kGap);  // 6 missing
  EXPECT_EQ(t.total_seq_gaps(), 4U);
}

TEST(BrokerStreamTracker, PublisherRestartIsNotAGap) {
  StreamTracker t("arm.joint");
  for (std::uint64_t s = 0; s < 1000; ++s) {
    t.on_message(s, 1, 1);
  }
  EXPECT_EQ(t.on_message(15, 1, 1), StreamTracker::SeqEvent::kRestart);
  EXPECT_EQ(t.on_message(16, 1, 1), StreamTracker::SeqEvent::kInOrder);
  EXPECT_EQ(t.total_seq_gaps(), 0U);
  EXPECT_EQ(t.restarts(), 1U);
}

TEST(BrokerStreamTracker, SnapshotReportsWindowThenResetsIt) {
  StreamTracker t("plc.conveyor");
  for (std::uint64_t s = 0; s < 100; ++s) {
    t.on_message(s, (s + 1) * 1000, 20);  // latencies 1..100 us
  }
  StreamStats w1;
  t.snapshot(kSec, /*total_drops=*/3, w1);
  EXPECT_EQ(w1.stream(), "plc.conveyor");
  EXPECT_EQ(w1.count(), 100U);
  EXPECT_DOUBLE_EQ(w1.rate_hz(), 100.0);
  EXPECT_EQ(w1.bytes(), 2000U);
  EXPECT_EQ(w1.drops(), 3U);
  EXPECT_EQ(w1.total_drops(), 3U);
  EXPECT_EQ(w1.max_ns(), 100'000U);
  EXPECT_NEAR(static_cast<double>(w1.p50_ns()), 50'000.0, 50'000.0 * 0.04);
  EXPECT_LE(w1.p50_ns(), w1.p99_ns());
  EXPECT_LE(w1.p99_ns(), w1.p999_ns());
  EXPECT_LE(w1.p999_ns(), w1.max_ns());

  // Half-second window with one gap; drops are reported as the increase.
  t.on_message(101, 5000, 20);
  StreamStats w2;
  t.snapshot(kSec / 2, /*total_drops=*/5, w2);
  EXPECT_EQ(w2.count(), 1U);
  EXPECT_DOUBLE_EQ(w2.rate_hz(), 2.0);
  EXPECT_EQ(w2.seq_gaps(), 1U);
  EXPECT_EQ(w2.drops(), 2U);
  EXPECT_EQ(w2.total_count(), 101U);
  EXPECT_EQ(w2.total_seq_gaps(), 1U);
  EXPECT_EQ(w2.total_drops(), 5U);
}

TEST(BrokerStreamTracker, EmptyWindowStillReported) {
  StreamTracker t("sensor.frame");
  StreamStats w;
  t.snapshot(kSec, 0, w);
  EXPECT_EQ(w.stream(), "sensor.frame");
  EXPECT_EQ(w.count(), 0U);
  EXPECT_DOUBLE_EQ(w.rate_hz(), 0.0);
  EXPECT_EQ(w.p50_ns(), 0U);
  EXPECT_EQ(w.p999_ns(), 0U);
  EXPECT_EQ(w.max_ns(), 0U);
}

// ------------------------------------------------------------ HealthTable ---

class BrokerHealthTable : public ::testing::Test {
 protected:
  static constexpr std::uint64_t kT0 = 10 * kSec;
  static constexpr std::uint64_t kTimeout = kSec;
  HealthTable table_{{"arm_sim", "plc_bridge"}, kTimeout, kT0};

  bool fill(std::uint64_t now, google::protobuf::RepeatedPtrField<ProcessHealth>& out) const {
    out.Clear();
    return table_.fill(now, out);
  }
};

TEST_F(BrokerHealthTable, NeverSeenProcessIsDeadAndUnspecified) {
  google::protobuf::RepeatedPtrField<ProcessHealth> out;
  EXPECT_FALSE(fill(kT0 + 300 * kMs, out));
  ASSERT_EQ(out.size(), 2);
  EXPECT_EQ(out[0].name(), "arm_sim");
  EXPECT_FALSE(out[0].alive());
  EXPECT_EQ(out[0].status(), PROCESS_STATUS_UNSPECIFIED);
  EXPECT_EQ(out[0].last_seen_age_ms(), 300U);
}

TEST_F(BrokerHealthTable, HealthyOnlyWhenAllAliveAndOk) {
  google::protobuf::RepeatedPtrField<ProcessHealth> out;
  EXPECT_EQ(table_.on_heartbeat("arm_sim", PROCESS_OK, 100, "", kT0), HealthTable::Event::kFirst);
  EXPECT_FALSE(fill(kT0, out));  // plc_bridge not seen yet
  table_.on_heartbeat("plc_bridge", PROCESS_DEGRADED, 100, "modbus link down", kT0);
  EXPECT_FALSE(fill(kT0, out));  // alive but degraded
  EXPECT_TRUE(out[1].alive());
  EXPECT_EQ(out[1].detail(), "modbus link down");
  EXPECT_EQ(table_.on_heartbeat("plc_bridge", PROCESS_OK, 300, "", kT0 + 200 * kMs),
            HealthTable::Event::kUpdated);
  EXPECT_TRUE(fill(kT0 + 200 * kMs, out));
  EXPECT_EQ(out[1].uptime_ms(), 300U);
  EXPECT_TRUE(out[1].detail().empty());
}

TEST_F(BrokerHealthTable, TimeoutBoundaryIsInclusive) {
  table_.on_heartbeat("arm_sim", PROCESS_OK, 1, "", kT0);
  EXPECT_TRUE(table_.alive("arm_sim", kT0 + kTimeout));
  EXPECT_FALSE(table_.alive("arm_sim", kT0 + kTimeout + 1));
  EXPECT_FALSE(table_.alive("nobody", kT0));
}

TEST_F(BrokerHealthTable, DetectsRestartAndIgnoresUnknownSources) {
  table_.on_heartbeat("arm_sim", PROCESS_OK, 5000, "", kT0);
  EXPECT_EQ(table_.on_heartbeat("arm_sim", PROCESS_STARTING, 200, "", kT0 + kSec),
            HealthTable::Event::kRestarted);
  EXPECT_EQ(table_.on_heartbeat("intruder", PROCESS_OK, 1, "", kT0),
            HealthTable::Event::kUnknownSource);
  EXPECT_EQ(table_.size(), 2U);
}

// ----------------------------------------------------------- CommandQueue ---

PendingCommand make_cmd(std::uint64_t id, std::uint64_t deadline_ns) {
  PendingCommand c;
  c.id = id;
  c.envelope = {"client-" + std::to_string(id), ""};
  c.payload = "payload-" + std::to_string(id);
  c.deadline_ns = deadline_ns;
  return c;
}

TEST(BrokerCommandQueue, OneInFlightRestQueuedFifo) {
  CommandQueue q(8);
  ASSERT_TRUE(q.submit(make_cmd(1, 100)));
  ASSERT_TRUE(q.submit(make_cmd(2, 100)));
  const PendingCommand* first = q.start_next(0);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->id, 1U);
  EXPECT_EQ(q.start_next(0), nullptr);  // 1 still on the wire
  EXPECT_TRUE(q.in_flight());
  EXPECT_EQ(q.queued(), 1U);

  const auto done = q.complete();
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(done->id, 1U);
  EXPECT_EQ(done->envelope.front(), "client-1");
  const PendingCommand* second = q.start_next(0);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->id, 2U);
}

TEST(BrokerCommandQueue, StrayReplyIsDropped) {
  CommandQueue q(8);
  EXPECT_FALSE(q.complete().has_value());
}

TEST(BrokerCommandQueue, RejectsBeyondLimit) {
  CommandQueue q(2);
  EXPECT_TRUE(q.submit(make_cmd(1, 100)));
  q.start_next(0);
  EXPECT_TRUE(q.submit(make_cmd(2, 100)));
  EXPECT_FALSE(q.submit(make_cmd(3, 100)));  // in-flight + queued == limit
  EXPECT_EQ(q.pending(), 2U);
}

TEST(BrokerCommandQueue, InFlightTimeoutDemandsLinkReset) {
  CommandQueue q(8);
  q.submit(make_cmd(1, 100));
  q.submit(make_cmd(2, 500));
  q.start_next(0);

  EXPECT_TRUE(q.expire(99).commands.empty());  // not yet
  const auto expired = q.expire(100);          // deadline is inclusive
  ASSERT_EQ(expired.commands.size(), 1U);
  EXPECT_EQ(expired.commands[0].id, 1U);
  EXPECT_TRUE(expired.reset_link);
  EXPECT_FALSE(q.in_flight());

  // The late reply to request 1 must not be matched to request 2: after the
  // reset nothing is in flight until request 2 is started on the new socket.
  EXPECT_FALSE(q.complete().has_value());
  const PendingCommand* next = q.start_next(100);
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(next->id, 2U);
}

TEST(BrokerCommandQueue, QueuedTimeoutNeedsNoReset) {
  CommandQueue q(8);
  q.submit(make_cmd(1, 1000));
  q.submit(make_cmd(2, 50));  // expires while waiting behind 1
  q.start_next(0);
  const auto expired = q.expire(60);
  ASSERT_EQ(expired.commands.size(), 1U);
  EXPECT_EQ(expired.commands[0].id, 2U);
  EXPECT_FALSE(expired.reset_link);
  EXPECT_TRUE(q.in_flight());
}

TEST(BrokerCommandQueue, NextDeadlineIsEarliestPending) {
  CommandQueue q(8);
  EXPECT_FALSE(q.next_deadline().has_value());
  q.submit(make_cmd(1, 900));
  q.submit(make_cmd(2, 300));
  q.start_next(0);
  ASSERT_TRUE(q.next_deadline().has_value());
  EXPECT_EQ(*q.next_deadline(), 300U);
}

// -------------------------------------------------------------- FrameScan ---

TEST(BrokerFrameScan, ReadsMetadataWithoutParsingCloud) {
  PointCloudFrame frame;
  fill_header(*frame.mutable_h(), 42, "sensor_sim");
  frame.set_width(640);
  frame.set_height(480);
  frame.set_points(std::string(640 * 480 * 12, '\x01'));
  const std::string wire = frame.SerializeAsString();

  FrameInfo info;
  ASSERT_TRUE(scan_frame_info(wire.data(), wire.size(), info));
  EXPECT_EQ(info.h().seq(), 42U);
  EXPECT_EQ(info.h().source(), "sensor_sim");
  EXPECT_EQ(info.h().send_mono_ns(), frame.h().send_mono_ns());
  EXPECT_EQ(info.width(), 640U);
  EXPECT_EQ(info.height(), 480U);
  EXPECT_EQ(info.bytes(), 640U * 480U * 12U);
}

TEST(BrokerFrameScan, RejectsMalformedInput) {
  FrameInfo info;
  EXPECT_FALSE(scan_frame_info(nullptr, 0, info));

  PointCloudFrame no_header;
  no_header.set_width(4);
  const std::string wire = no_header.SerializeAsString();
  EXPECT_FALSE(scan_frame_info(wire.data(), wire.size(), info));

  PointCloudFrame frame;
  fill_header(*frame.mutable_h(), 1, "sensor_sim");
  frame.set_points(std::string(1000, 'x'));
  const std::string full = frame.SerializeAsString();
  EXPECT_FALSE(scan_frame_info(full.data(), full.size() - 10, info));  // truncated cloud
}

// ------------------------------------------------------------ Downsampler ---

TEST(BrokerDownsampler, OneKilohertzInGivesExactlyFiftyHertzOut) {
  Downsampler<int> ds(20 * kMs);
  std::vector<int> out;
  for (int i = 0; i < 1000; ++i) {  // one second of 1 kHz input
    const std::uint64_t now = static_cast<std::uint64_t>(i) * kMs;
    ds.push(i);
    if (auto v = ds.poll(now)) {
      out.push_back(*v);
    }
  }
  EXPECT_EQ(out.size(), 50U);
  EXPECT_EQ(out.front(), 0);  // first value goes out immediately
  EXPECT_EQ(out[1], 20);      // always the freshest sample at the tick
  EXPECT_EQ(ds.received(), 1000U);
  EXPECT_EQ(ds.emitted(), 50U);
}

TEST(BrokerDownsampler, EmitsNothingWithoutNewInput) {
  Downsampler<int> ds(20 * kMs);
  EXPECT_FALSE(ds.poll(0).has_value());
  ds.push(1);
  EXPECT_TRUE(ds.poll(0).has_value());
  EXPECT_FALSE(ds.poll(100 * kMs).has_value());  // nothing new since
  ds.push(2);
  EXPECT_TRUE(ds.poll(100 * kMs).has_value());  // input resumed: emitted at once
  ds.push(3);
  EXPECT_FALSE(ds.poll(110 * kMs).has_value());  // grid restarted at 100 ms
  EXPECT_TRUE(ds.poll(120 * kMs).has_value());
}

TEST(BrokerPeriodicTimer, FixedGridAndNoBurstAfterStall) {
  PeriodicTimer timer(10 * kMs, 0);
  EXPECT_FALSE(timer.due(9 * kMs));
  EXPECT_TRUE(timer.due(11 * kMs));   // late by 1 ms...
  EXPECT_TRUE(timer.due(20 * kMs));   // ...but the next tick stays on the grid
  EXPECT_TRUE(timer.due(95 * kMs));   // long stall: fires once
  EXPECT_FALSE(timer.due(96 * kMs));  // and does not burst to catch up
  EXPECT_TRUE(timer.due(105 * kMs));
}

// --------------------------------------------------------------- Liveness ---

TEST(BrokerLiveness, KickOnlyWhenEveryThreadAdvanced) {
  LivenessMonitor mon(3, 0);
  EXPECT_TRUE(mon.update({1, 1, 1}, 100 * kMs));
  EXPECT_FALSE(mon.update({2, 2, 1}, 200 * kMs));  // thread 2 has not moved
  EXPECT_FALSE(mon.update({3, 3, 1}, 300 * kMs));
  EXPECT_TRUE(mon.update({3, 3, 2}, 400 * kMs));  // all moved since the last kick
  EXPECT_FALSE(mon.update({3, 3, 2}, 500 * kMs));
}

TEST(BrokerLiveness, ReportsStalledThreads) {
  LivenessMonitor mon(2, 0);
  mon.update({1, 1}, 100 * kMs);
  mon.update({2, 1}, 2500 * kMs);
  const auto stalled = mon.stalled(2500 * kMs, 2 * kSec);
  ASSERT_EQ(stalled.size(), 1U);
  EXPECT_EQ(stalled[0], 1U);
  EXPECT_TRUE(mon.stalled(2500 * kMs, 3 * kSec).empty());
}

}  // namespace
}  // namespace cell::broker
