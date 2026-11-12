// Broker I/O thread: the latency-critical path. It owns every SUB socket and
// does the minimum per message - timestamp, header decode, ring push - so that
// the receive timestamp is not delayed by anything the broker does with the
// data afterwards. It never blocks on the ring, never parses a message body
// and never touches the consumer or command sockets.
#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zmq.hpp>

#include "broker/shared.hpp"
#include "cell.pb.h"
#include "common/clock.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/zmq_util.hpp"

namespace cell::broker {
namespace {

struct Subscription {
  zmq::socket_t socket;
  std::string_view topic;  // empty for heartbeat-only entries; views BrokerConfig
  std::uint16_t index = 0;
};

// Non-blocking receive of one frame; false when nothing is waiting.
bool recv_frame(zmq::socket_t& socket, zmq::message_t& frame, zmq::recv_flags flags) {
  try {
    return socket.recv(frame, flags).has_value();
  } catch (const zmq::error_t& e) {
    if (e.num() == EINTR) {
      return false;
    }
    throw;
  }
}

// Take up to `batch` messages from one readable socket. The bound keeps a
// flooding stream from starving the others: zmq::poll is level-triggered, so
// whatever is left makes the next poll return immediately and every readable
// socket gets its turn in between.
void drain(Shared& shared, Subscription& sub, Header& header) {
  for (std::size_t n = 0; n < shared.cfg.io_drain_batch; ++n) {
    zmq::message_t topic;
    if (!recv_frame(sub.socket, topic, zmq::recv_flags::dontwait)) {
      return;
    }
    // Stamp first: everything after this line is broker overhead that must not
    // be counted as transport latency. (A multipart message is delivered
    // atomically, so the payload frame is already here.)
    const std::uint64_t recv_ns = mono_now_ns();

    zmq::message_t payload;
    bool well_formed = topic.more() && recv_frame(sub.socket, payload, zmq::recv_flags::none);
    while (well_formed && payload.more()) {  // more than [topic][payload]: discard all of it
      zmq::message_t extra;
      if (!recv_frame(sub.socket, extra, zmq::recv_flags::none) || !extra.more()) {
        well_formed = false;
      }
    }

    const std::string_view name = frame_view(topic);
    ItemKind kind = ItemKind::kData;
    if (name == topic::kHeartbeat) {
      kind = ItemKind::kHeartbeat;
    } else if (sub.topic.empty() || name != sub.topic) {
      well_formed = false;  // subscriptions are prefix matches: "arm.joint2" lands here
    }
    // Header only: field 1 of every cell message. A PointCloudFrame body is
    // megabytes; it is never decoded on this thread.
    if (!well_formed || !parse_header_only(payload.data(), payload.size(), header)) {
      shared.malformed.add();
      continue;
    }

    RingItem item;
    item.seq = header.seq();
    item.send_ns = header.send_mono_ns();
    item.recv_ns = recv_ns;
    item.size = static_cast<std::uint32_t>(payload.size());
    item.stream = sub.index;
    item.kind = kind;
    item.payload = std::move(payload);
    if (!shared.ring.try_push(std::move(item))) {
      // Ring full: the processing thread is behind. Dropping here (and
      // counting it) keeps this thread, and so the latency measurement of
      // every other stream, unaffected; blocking would turn one slow consumer
      // into back-pressure on all publishers.
      (kind == ItemKind::kData ? shared.drops[sub.index] : shared.hb_drops).add();
    }
  }
}

}  // namespace

void run_io_thread(Shared& shared) {
  const BrokerConfig& cfg = shared.cfg;
  std::vector<Subscription> subs;
  std::vector<zmq::pollitem_t> items;
  subs.reserve(cfg.streams.size());
  for (std::size_t i = 0; i < cfg.streams.size(); ++i) {
    const StreamConfig& stream = cfg.streams[i];
    std::vector<std::string> topics{topic::kHeartbeat};
    if (stream.has_data()) {
      topics.push_back(stream.topic);
    }
    SocketOptions opts;
    opts.rcvhwm = cfg.sub_rcvhwm;
    subs.push_back({make_sub(shared.ctx, stream.endpoint, topics, opts), stream.topic,
                    static_cast<std::uint16_t>(i)});
    items.push_back({subs.back().socket.handle(), 0, ZMQ_POLLIN, 0});
    spdlog::info("subscribed to {} [{}] from {}", stream.endpoint,
                 stream.has_data() ? stream.topic + ", hb" : std::string("hb"), stream.process);
  }
  shared.io.ready.store(true);

  const std::chrono::milliseconds timeout(cfg.poll_timeout_ms);
  Header header;  // reused: parsing into it does not allocate once source has capacity
  while (!shutdown_requested()) {
    shared.io.beats.fetch_add(1, std::memory_order_relaxed);
    int ready = 0;
    try {
      ready = zmq::poll(items.data(), items.size(), timeout);
    } catch (const zmq::error_t& e) {
      if (e.num() != EINTR) {
        throw;
      }
    }
    if (ready <= 0) {
      continue;
    }
    for (std::size_t i = 0; i < subs.size(); ++i) {
      if ((items[i].revents & ZMQ_POLLIN) != 0) {
        drain(shared, subs[i], header);
      }
    }
  }
}

}  // namespace cell::broker
