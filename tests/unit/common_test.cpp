#include <gtest/gtest.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "common/clock.hpp"
#include "common/config.hpp"
#include "common/heartbeat.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/zmq_util.hpp"

using namespace std::chrono_literals;

namespace cell {
namespace {

TEST(Clock, IsMonotonic) {
  const auto a = mono_now_ns();
  const auto b = mono_now_ns();
  EXPECT_GE(b, a);
  EXPECT_TRUE(sleep_until_ns(mono_now_ns() + 1'000'000));
  EXPECT_GE(mono_now_ns() - a, 1'000'000U);
}

TEST(Config, DottedLookupAndDefaults) {
  const Json cfg = Json::parse(R"({"a": {"b": {"c": 7}}, "name": "x"})");
  EXPECT_EQ(cfg_get<int>(cfg, "a.b.c"), 7);
  EXPECT_EQ(cfg_get<std::string>(cfg, "name"), "x");
  EXPECT_EQ(cfg_get_or<int>(cfg, "a.missing", 42), 42);
  EXPECT_EQ(cfg_find(cfg, "a.b.c.d"), nullptr);
  EXPECT_THROW(cfg_get<int>(cfg, "nope"), ConfigError);
  EXPECT_THROW(cfg_get<int>(cfg, "name"), ConfigError);  // wrong type
}

TEST(Config, OverridesParseJsonThenFallBackToString) {
  Json cfg = Json::parse(R"({"rate_hz": 1000, "modbus": {"host": "a"}})");
  cfg_apply_override(cfg, "rate_hz=2000");
  cfg_apply_override(cfg, "modbus.host=10.0.0.5");
  cfg_apply_override(cfg, "modbus.port=5020");
  cfg_apply_override(cfg, "new.flag=true");
  cfg_apply_override(cfg, "endpoint=ipc:///tmp/x=y");
  EXPECT_EQ(cfg_get<int>(cfg, "rate_hz"), 2000);
  EXPECT_EQ(cfg_get<std::string>(cfg, "modbus.host"), "10.0.0.5");
  EXPECT_EQ(cfg_get<int>(cfg, "modbus.port"), 5020);
  EXPECT_TRUE(cfg_get<bool>(cfg, "new.flag"));
  EXPECT_EQ(cfg_get<std::string>(cfg, "endpoint"), "ipc:///tmp/x=y");
  EXPECT_THROW(cfg_apply_override(cfg, "novalue"), ConfigError);
  EXPECT_THROW(cfg_apply_override(cfg, "=1"), ConfigError);
}

TEST(Config, LoadsFileWithComments) {
  const auto path =
      std::filesystem::temp_directory_path() / ("cell_cfg_" + std::to_string(::getpid()) + ".json");
  {
    std::ofstream out(path);
    out << "{ // comment\n \"k\": 1 }\n";
  }
  EXPECT_EQ(cfg_get<int>(load_config_file(path), "k"), 1);
  std::filesystem::remove(path);
  EXPECT_THROW(load_config_file(path), ConfigError);
}

TEST(Signals, SigtermSetsFlag) {
  install_shutdown_handlers();
  reset_shutdown_flag();
  EXPECT_FALSE(shutdown_requested());
  ASSERT_EQ(::raise(SIGTERM), 0);
  EXPECT_TRUE(shutdown_requested());
  reset_shutdown_flag();
}

TEST(ZmqUtil, HeaderOnlyParseSkipsPayload) {
  PointCloudFrame frame;
  fill_header(*frame.mutable_h(), 123, "sensor_sim");
  frame.set_width(640);
  frame.set_height(480);
  frame.set_points(std::string(640 * 480 * 12, '\x7f'));
  const std::string wire = frame.SerializeAsString();

  Header h;
  ASSERT_TRUE(parse_header_only(wire.data(), wire.size(), h));
  EXPECT_EQ(h.seq(), 123U);
  EXPECT_EQ(h.source(), "sensor_sim");
  EXPECT_EQ(h.send_mono_ns(), frame.h().send_mono_ns());

  // Header found even when it is not the first field on the wire.
  JointState js;
  js.add_position(1.0);
  std::string reordered = js.SerializeAsString();
  JointState only_header;
  fill_header(*only_header.mutable_h(), 9, "arm_sim");
  reordered += only_header.SerializeAsString();
  ASSERT_TRUE(parse_header_only(reordered.data(), reordered.size(), h));
  EXPECT_EQ(h.seq(), 9U);

  EXPECT_FALSE(parse_header_only(wire.data(), 3, h));  // truncated
  EXPECT_FALSE(parse_header_only(nullptr, 0, h));
}

TEST(ZmqUtil, PubSubRoundTripWithHeartbeat) {
  zmq::context_t ctx;
  const std::string endpoint = "inproc://common_test_pubsub";
  auto pub = make_pub(ctx, endpoint);
  auto sub = make_sub(ctx, endpoint, {topic::kHeartbeat});

  HeartbeatEmitter hb("unit_test", 10);
  hb.set_status(PROCESS_OK);
  std::vector<zmq::message_t> frames;
  // PUB/SUB joins are asynchronous: keep publishing until the first one lands.
  bool got = false;
  for (int i = 0; i < 200 && !got; ++i) {
    hb.publish_now(pub, mono_now_ns());
    got = poll_in(sub, 10ms) && recv_multipart(sub, frames, /*dontwait=*/true);
  }
  ASSERT_TRUE(got);
  ASSERT_EQ(frames.size(), 2U);
  EXPECT_EQ(frame_view(frames[0]), topic::kHeartbeat);
  Heartbeat msg;
  ASSERT_TRUE(parse_proto(frames[1], msg));
  EXPECT_EQ(msg.h().source(), "unit_test");
  EXPECT_EQ(msg.status(), PROCESS_OK);
  EXPECT_GT(msg.h().send_mono_ns(), 0U);
}

TEST(ZmqUtil, ReqRepRoundTripAndTimeout) {
  zmq::context_t ctx;
  const std::string endpoint = "inproc://common_test_reqrep";
  auto rep = make_rep(ctx, endpoint);
  auto req = make_req(ctx, endpoint);

  Command cmd;
  fill_header(*cmd.mutable_h(), 1, "test");
  cmd.set_target("arm");
  cmd.mutable_move_to()->add_joints(0.5);
  ASSERT_TRUE(send_proto(req, cmd));

  Command got;
  ASSERT_TRUE(recv_proto(rep, got, 1000ms));
  EXPECT_EQ(got.action_case(), Command::kMoveTo);
  CommandReply reply;
  reply.set_ok(true);
  ASSERT_TRUE(send_proto(rep, reply));
  CommandReply back;
  ASSERT_TRUE(recv_proto(req, back, 1000ms));
  EXPECT_TRUE(back.ok());

  // No reply: recv times out, and REQ_RELAXED lets the next request go out.
  ASSERT_TRUE(send_proto(req, cmd));
  EXPECT_FALSE(recv_proto(req, back, 20ms));
  EXPECT_TRUE(send_proto(req, cmd));
}

TEST(ZmqUtil, HeartbeatTickHonoursInterval) {
  zmq::context_t ctx;
  auto pub = make_pub(ctx, "inproc://common_test_hb_tick");
  HeartbeatEmitter hb("unit_test", 100);
  const std::uint64_t t0 = mono_now_ns();
  EXPECT_TRUE(hb.tick(pub, t0));
  EXPECT_FALSE(hb.tick(pub, t0 + 50'000'000));
  EXPECT_TRUE(hb.tick(pub, t0 + 100'000'000));
}

}  // namespace
}  // namespace cell
