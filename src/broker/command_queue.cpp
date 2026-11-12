#include "broker/command_queue.hpp"

#include <algorithm>
#include <utility>

namespace cell::broker {

bool CommandQueue::submit(PendingCommand cmd) {
  if (pending() >= limit_) {
    return false;
  }
  queue_.push_back(std::move(cmd));
  return true;
}

const PendingCommand* CommandQueue::start_next(std::uint64_t now_ns) {
  if (in_flight_.has_value() || queue_.empty() || now_ns >= queue_.front().deadline_ns) {
    return nullptr;
  }
  in_flight_.emplace(std::move(queue_.front()));
  queue_.pop_front();
  return &*in_flight_;
}

std::optional<PendingCommand> CommandQueue::complete() {
  std::optional<PendingCommand> done = std::move(in_flight_);
  in_flight_.reset();
  return done;
}

CommandQueue::Expired CommandQueue::expire(std::uint64_t now_ns) {
  Expired out;
  if (in_flight_.has_value() && now_ns >= in_flight_->deadline_ns) {
    out.commands.push_back(std::move(*in_flight_));
    in_flight_.reset();
    out.reset_link = true;
  }
  // Deadlines are normally FIFO-ordered, but nothing here relies on it.
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (now_ns >= it->deadline_ns) {
      out.commands.push_back(std::move(*it));
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

std::optional<std::uint64_t> CommandQueue::next_deadline() const {
  std::optional<std::uint64_t> earliest;
  if (in_flight_.has_value()) {
    earliest = in_flight_->deadline_ns;
  }
  for (const auto& cmd : queue_) {
    earliest = earliest.has_value() ? std::min(*earliest, cmd.deadline_ns) : cmd.deadline_ns;
  }
  return earliest;
}

}  // namespace cell::broker
