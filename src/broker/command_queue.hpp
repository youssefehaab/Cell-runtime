#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace cell::broker {

// One client request waiting for a device.
struct PendingCommand {
  std::uint64_t id = 0;               // broker-local, for logs
  std::vector<std::string> envelope;  // ROUTER routing frames, replayed on the reply
  std::string payload;                // serialized Command, forwarded unchanged
  std::uint64_t deadline_ns = 0;      // receipt time + command_timeout
};

// Pending-request table for ONE command target (one device REP socket). Pure
// bookkeeping: no sockets and no clock, so the timeout rules are unit-testable.
//
// Why "at most one in flight":
// A device REP socket answers one request at a time, in order, and its reply
// carries nothing that identifies the request. The only way to know which
// request a reply belongs to is therefore to make sure there is exactly one
// candidate. So per target:
//   * at most one request is on the wire (in flight); the rest wait here in
//     FIFO order, each with its own deadline counted from when the broker
//     received it (so a client always hears back within command_timeout, queue
//     time included);
//   * a reply that arrives while a request is in flight completes exactly that
//     request;
//   * when the in-flight request times out, expire() reports `reset_link` and
//     the owner must close and recreate the DEALER for this target before
//     sending anything else. Closing the socket discards both the unsent
//     request (device down: it must not be executed minutes later when the
//     device comes back, after the client was told "timeout") and any late
//     reply (device slow: request A's reply must not be handed to the client
//     of request B). After the reset the new socket has carried nothing, so
//     the one-candidate invariant holds again.
// Requests that expire while still queued were never sent, so they need no
// reset: they are simply answered with a timeout.
class CommandQueue {
 public:
  explicit CommandQueue(std::size_t limit) : limit_(limit == 0 ? 1 : limit) {}

  // False if in-flight + queued already equals the limit (caller replies "busy").
  bool submit(PendingCommand cmd);

  // If nothing is in flight and the oldest queued request has not expired,
  // mark it in flight and return it so the caller can put it on the wire.
  // Expired requests are left for expire(), which must be called first.
  const PendingCommand* start_next(std::uint64_t now_ns);

  // A reply arrived on this target's socket. Returns the request it answers,
  // or nullopt if nothing is in flight (a stray frame: drop it).
  std::optional<PendingCommand> complete();

  struct Expired {
    std::vector<PendingCommand> commands;  // answer each with a timeout error
    bool reset_link = false;               // the in-flight request was among them
  };
  // Remove every request whose deadline has passed (now >= deadline).
  Expired expire(std::uint64_t now_ns);

  // Earliest deadline of any pending request, for the poll timeout.
  std::optional<std::uint64_t> next_deadline() const;

  bool in_flight() const noexcept { return in_flight_.has_value(); }
  std::size_t queued() const noexcept { return queue_.size(); }
  std::size_t pending() const noexcept { return queue_.size() + (in_flight_ ? 1 : 0); }

 private:
  std::size_t limit_;
  std::optional<PendingCommand> in_flight_;
  std::deque<PendingCommand> queue_;
};

}  // namespace cell::broker
