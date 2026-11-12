// Broker command thread: external ROUTER in front, one DEALER per device REP
// socket behind it.
//
// Why its own thread: the alternatives are the I/O thread or the processing
// thread. The I/O thread stamps receive times, so anything else it does shows
// up as measured latency - and command handling includes a full Command parse
// and, on a timeout, closing and re-creating a socket (syscalls, a mailbox
// round trip to the zmq I/O thread). The processing thread is not poll-driven:
// it sleeps on the ring, so commands would wait for its next wake-up and it
// would have to poll these sockets thousands of times a second for traffic that
// arrives a few times a minute. A third thread that sleeps in zmq::poll costs
// nothing while idle, reacts to a command immediately, and keeps every command
// socket on exactly one thread. It follows the same liveness rule as the other
// two: the watchdog is only kicked while this loop keeps turning.
//
// Nothing here ever waits for a device: requests are sent, remembered in a
// CommandQueue (see command_queue.hpp for the correlation rules) and answered
// when the reply or the deadline comes, whichever is first.
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <zmq.hpp>

#include "broker/command_queue.hpp"
#include "broker/shared.hpp"
#include "cell.pb.h"
#include "common/clock.hpp"
#include "common/signals.hpp"
#include "common/topics.hpp"
#include "common/zmq_util.hpp"

namespace cell::broker {
namespace {

constexpr std::uint64_t kNsPerMs = 1'000'000ULL;
// Client requests accepted per loop turn, so a flood of commands cannot keep
// the loop from expiring deadlines and relaying replies.
constexpr std::size_t kRequestBatch = 32;

struct Route {
  std::string target;
  std::string endpoint;
  zmq::socket_t dealer;
  CommandQueue queue;
};

zmq::socket_t make_dealer(zmq::context_t& ctx, const std::string& endpoint) {
  zmq::socket_t sock(ctx, zmq::socket_type::dealer);
  sock.set(zmq::sockopt::linger, 0);  // closing must discard unsent requests
  sock.connect(endpoint);
  return sock;
}

zmq::socket_t make_router(zmq::context_t& ctx, const std::string& endpoint) {
  zmq::socket_t sock(ctx, zmq::socket_type::router);
  sock.set(zmq::sockopt::linger, 0);
  ensure_ipc_dir(endpoint);
  sock.bind(endpoint);
  return sock;
}

class CommandRouter {
 public:
  explicit CommandRouter(Shared& shared)
      : shared_(shared),
        cfg_(shared.cfg),
        router_(make_router(shared.ctx, cfg_.cmd_endpoint)),
        timeout_ns_(cfg_.command_timeout_ms * kNsPerMs) {
    for (const auto& [target, endpoint] : cfg_.routes) {
      routes_.push_back({target, endpoint, make_dealer(shared.ctx, endpoint),
                         CommandQueue(cfg_.command_queue_limit)});
      if (!valid_targets_.empty()) {
        valid_targets_ += ", ";
      }
      valid_targets_ += target;
      spdlog::info("route '{}' -> {}", target, endpoint);
    }
    spdlog::info("accepting commands on {} (timeout {} ms)", cfg_.cmd_endpoint,
                 cfg_.command_timeout_ms);
  }

  void run() {
    std::vector<zmq::pollitem_t> items;
    while (!shutdown_requested()) {
      shared_.command.beats.fetch_add(1, std::memory_order_relaxed);

      // Rebuilt every turn: a DEALER may have been replaced since the last one.
      items.clear();
      items.push_back({router_.handle(), 0, ZMQ_POLLIN, 0});
      for (Route& route : routes_) {
        items.push_back({route.dealer.handle(), 0, ZMQ_POLLIN, 0});
      }
      try {
        zmq::poll(items.data(), items.size(), poll_timeout(mono_now_ns()));
      } catch (const zmq::error_t& e) {
        if (e.num() != EINTR) {
          throw;
        }
        continue;
      }

      // Order matters: replies first, so one that arrived just before its
      // deadline is delivered rather than expired; then new requests; then
      // deadlines; and last, with the queues settled, the next sends.
      for (std::size_t i = 0; i < routes_.size(); ++i) {
        if ((items[i + 1].revents & ZMQ_POLLIN) != 0) {
          relay_replies(routes_[i]);
        }
      }
      if ((items[0].revents & ZMQ_POLLIN) != 0) {
        accept_requests();
      }
      const std::uint64_t now = mono_now_ns();
      for (Route& route : routes_) {
        expire(route, now);
        dispatch(route, now);
      }
    }
  }

 private:
  // Sleep until the next deadline, but never longer than poll_timeout_ms.
  std::chrono::milliseconds poll_timeout(std::uint64_t now) const {
    std::uint64_t wait_ms = cfg_.poll_timeout_ms;
    for (const Route& route : routes_) {
      if (const auto deadline = route.queue.next_deadline()) {
        const std::uint64_t left_ns = *deadline > now ? *deadline - now : 0;
        // Round up: waking a fraction of a millisecond early would just spin.
        wait_ms = std::min(wait_ms, (left_ns + kNsPerMs - 1) / kNsPerMs);
      }
    }
    return std::chrono::milliseconds(wait_ms);
  }

  void accept_requests() {
    std::vector<zmq::message_t> frames;
    for (std::size_t n = 0; n < kRequestBatch && recv_multipart(router_, frames, true); ++n) {
      if (frames.size() < 2) {
        continue;  // identity only: nothing to answer
      }
      // Last frame = Command; everything before it is the routing envelope
      // ([identity] for a DEALER client, [identity][empty] for a REQ client)
      // and is echoed back verbatim in front of the reply.
      PendingCommand pending;
      pending.id = next_id_++;
      for (std::size_t i = 0; i + 1 < frames.size(); ++i) {
        pending.envelope.emplace_back(frame_view(frames[i]));
      }
      Command cmd;
      if (!parse_proto(frames.back(), cmd)) {
        reply_error(pending.envelope, "malformed command: last frame is not a valid Command");
        continue;
      }
      const auto route = std::find_if(routes_.begin(), routes_.end(),
                                      [&](const Route& r) { return r.target == cmd.target(); });
      if (route == routes_.end()) {
        reply_error(pending.envelope, "unknown target '" + cmd.target() +
                                          "' (valid targets: " + valid_targets_ + ")");
        continue;
      }
      pending.payload.assign(frame_view(frames.back()));
      pending.deadline_ns = mono_now_ns() + timeout_ns_;
      std::vector<std::string> envelope = pending.envelope;  // submit() consumes `pending`
      if (!route->queue.submit(std::move(pending))) {
        reply_error(envelope, "busy: too many pending commands for '" + route->target + "'");
      }
    }
  }

  void relay_replies(Route& route) {
    std::vector<zmq::message_t> frames;
    while (recv_multipart(route.dealer, frames, true)) {
      // The REP side answers [empty][CommandReply]; the reply is the last frame.
      auto done = route.queue.complete();
      if (!done.has_value()) {
        // Cannot happen with a compliant REP peer (one reply per request, and
        // the socket is replaced after every timeout), so say so if it does.
        spdlog::warn("unexpected frame from '{}' with no request in flight; dropped", route.target);
        continue;
      }
      spdlog::debug("command #{} for '{}' answered", done->id, route.target);
      send_reply(done->envelope, frames.back());
    }
  }

  void expire(Route& route, std::uint64_t now) {
    CommandQueue::Expired expired = route.queue.expire(now);
    for (const PendingCommand& cmd : expired.commands) {
      spdlog::warn("command #{} for '{}' timed out after {} ms", cmd.id, route.target,
                   cfg_.command_timeout_ms);
      reply_error(cmd.envelope, "timeout waiting for '" + route.target + "'");
    }
    if (expired.reset_link) {
      // The timed-out request may still be sitting in this socket (device
      // down) or its reply may still arrive (device slow). A new socket makes
      // both impossible; see command_queue.hpp.
      route.dealer = make_dealer(shared_.ctx, route.endpoint);
    }
  }

  void dispatch(Route& route, std::uint64_t now) {
    const PendingCommand* next = route.queue.start_next(now);
    if (next == nullptr) {
      return;
    }
    try {
      // A DEALER must supply the empty delimiter a REQ would add; REP needs it.
      // If the device is not connected yet the request waits in the socket
      // until the device appears or the deadline replaces the socket.
      route.dealer.send(zmq::message_t(), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
      route.dealer.send(zmq::buffer(next->payload), zmq::send_flags::dontwait);
      spdlog::debug("command #{} forwarded to '{}'", next->id, route.target);
    } catch (const zmq::error_t& e) {
      // Left in flight: the deadline will answer the client and reset the link.
      spdlog::warn("sending command #{} to '{}' failed: {}", next->id, route.target, e.what());
    }
  }

  void reply_error(const std::vector<std::string>& envelope, const std::string& error) {
    CommandReply reply;
    reply.set_ok(false);
    reply.set_error(error);
    fill_header(*reply.mutable_h(), reply_seq_++, process::kBroker);
    zmq::message_t frame(reply.ByteSizeLong());
    reply.SerializeWithCachedSizesToArray(static_cast<std::uint8_t*>(frame.data()));
    spdlog::debug("command rejected: {}", error);
    send_reply(envelope, frame);
  }

  // [envelope...][reply]. A ROUTER silently drops messages for a client that
  // has gone away, which is what we want for a reply nobody is waiting for.
  void send_reply(const std::vector<std::string>& envelope, zmq::message_t& reply) {
    try {
      for (const std::string& frame : envelope) {
        router_.send(zmq::buffer(frame), zmq::send_flags::sndmore | zmq::send_flags::dontwait);
      }
      router_.send(reply, zmq::send_flags::dontwait);
    } catch (const zmq::error_t& e) {
      spdlog::warn("sending a command reply failed: {}", e.what());
    }
  }

  Shared& shared_;
  const BrokerConfig& cfg_;
  zmq::socket_t router_;
  std::uint64_t timeout_ns_;
  std::vector<Route> routes_;
  std::string valid_targets_;
  std::uint64_t next_id_ = 1;
  std::uint64_t reply_seq_ = 0;
};

}  // namespace

void run_command_thread(Shared& shared) {
  CommandRouter router(shared);
  shared.command.ready.store(true);
  router.run();
}

}  // namespace cell::broker
