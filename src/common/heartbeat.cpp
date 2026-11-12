#include "common/heartbeat.hpp"

#include <utility>

#include "common/clock.hpp"
#include "common/topics.hpp"
#include "common/zmq_util.hpp"

namespace cell {

HeartbeatEmitter::HeartbeatEmitter(std::string source, std::uint64_t interval_ms)
    : source_(std::move(source)),
      interval_ns_(interval_ms * 1'000'000ULL),
      start_ns_(mono_now_ns()) {}

void HeartbeatEmitter::set_status(ProcessStatus status, std::string detail) {
  status_ = status;
  detail_ = std::move(detail);
}

bool HeartbeatEmitter::tick(zmq::socket_t& pub, std::uint64_t now_mono_ns) {
  if (last_ns_ != 0 && now_mono_ns - last_ns_ < interval_ns_) {
    return false;
  }
  publish_now(pub, now_mono_ns);
  return true;
}

void HeartbeatEmitter::publish_now(zmq::socket_t& pub, std::uint64_t now_mono_ns) {
  last_ns_ = now_mono_ns;
  Heartbeat hb;
  fill_header(*hb.mutable_h(), seq_++, source_);
  hb.set_status(status_);
  hb.set_uptime_ms((now_mono_ns - start_ns_) / 1'000'000ULL);
  hb.set_detail(detail_);
  send_proto(pub, topic::kHeartbeat, hb);
}

}  // namespace cell
