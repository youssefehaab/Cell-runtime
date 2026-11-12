#include "common/zmq_util.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/wire_format_lite.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

#include "common/clock.hpp"

namespace cell {
namespace {

constexpr std::string_view kIpcPrefix = "ipc://";

// EINTR means a signal arrived (usually shutdown); treat as "no result".
template <typename Fn>
bool interruptible(Fn&& fn) {
  try {
    return fn();
  } catch (const zmq::error_t& e) {
    if (e.num() == EINTR) {
      return false;
    }
    throw;
  }
}

zmq::message_t serialize(const google::protobuf::MessageLite& msg) {
  const std::size_t size = msg.ByteSizeLong();
  zmq::message_t frame(size);
  msg.SerializeWithCachedSizesToArray(static_cast<std::uint8_t*>(frame.data()));
  return frame;
}

}  // namespace

void ensure_ipc_dir(const std::string& endpoint) {
  if (endpoint.rfind(kIpcPrefix, 0) != 0) {
    return;
  }
  const std::filesystem::path path(endpoint.substr(kIpcPrefix.size()));
  if (path.has_parent_path()) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
  }
}

zmq::socket_t make_pub(zmq::context_t& ctx, const std::string& bind_endpoint, SocketOptions opts) {
  zmq::socket_t sock(ctx, zmq::socket_type::pub);
  sock.set(zmq::sockopt::linger, 0);
  sock.set(zmq::sockopt::sndhwm, opts.sndhwm);
  ensure_ipc_dir(bind_endpoint);
  sock.bind(bind_endpoint);
  return sock;
}

zmq::socket_t make_sub(zmq::context_t& ctx, const std::string& endpoint,
                       const std::vector<std::string>& topics, SocketOptions opts) {
  zmq::socket_t sock(ctx, zmq::socket_type::sub);
  sock.set(zmq::sockopt::linger, 0);
  sock.set(zmq::sockopt::rcvhwm, opts.rcvhwm);
  for (const auto& t : topics) {
    sock.set(zmq::sockopt::subscribe, t);
  }
  sock.connect(endpoint);
  return sock;
}

zmq::socket_t make_rep(zmq::context_t& ctx, const std::string& bind_endpoint) {
  zmq::socket_t sock(ctx, zmq::socket_type::rep);
  sock.set(zmq::sockopt::linger, 0);
  ensure_ipc_dir(bind_endpoint);
  sock.bind(bind_endpoint);
  return sock;
}

zmq::socket_t make_req(zmq::context_t& ctx, const std::string& endpoint) {
  zmq::socket_t sock(ctx, zmq::socket_type::req);
  sock.set(zmq::sockopt::linger, 0);
  sock.set(zmq::sockopt::req_relaxed, 1);
  sock.set(zmq::sockopt::req_correlate, 1);
  sock.connect(endpoint);
  return sock;
}

bool send_proto(zmq::socket_t& sock, std::string_view topic,
                const google::protobuf::MessageLite& msg) {
  return interruptible([&] {
    zmq::message_t payload = serialize(msg);
    if (!sock.send(zmq::buffer(topic), zmq::send_flags::sndmore | zmq::send_flags::dontwait)) {
      return false;
    }
    return sock.send(payload, zmq::send_flags::dontwait).has_value();
  });
}

bool send_proto(zmq::socket_t& sock, const google::protobuf::MessageLite& msg) {
  return interruptible([&] {
    zmq::message_t payload = serialize(msg);
    return sock.send(payload, zmq::send_flags::dontwait).has_value();
  });
}

bool poll_in(zmq::socket_t& sock, std::chrono::milliseconds timeout) {
  return interruptible([&] {
    zmq::pollitem_t item{sock.handle(), 0, ZMQ_POLLIN, 0};
    return zmq::poll(&item, 1, timeout) > 0 && (item.revents & ZMQ_POLLIN) != 0;
  });
}

bool recv_multipart(zmq::socket_t& sock, std::vector<zmq::message_t>& frames, bool dontwait) {
  frames.clear();
  return interruptible([&] {
    const auto flags = dontwait ? zmq::recv_flags::dontwait : zmq::recv_flags::none;
    while (true) {
      zmq::message_t frame;
      // Only the first frame may be non-blocking; the rest of a multipart
      // message is delivered atomically.
      if (!sock.recv(frame, frames.empty() ? flags : zmq::recv_flags::none)) {
        frames.clear();
        return false;
      }
      const bool more = frame.more();
      frames.push_back(std::move(frame));
      if (!more) {
        return true;
      }
    }
  });
}

bool recv_proto(zmq::socket_t& sock, google::protobuf::MessageLite& msg,
                std::chrono::milliseconds timeout) {
  if (!poll_in(sock, timeout)) {
    return false;
  }
  std::vector<zmq::message_t> frames;
  if (!recv_multipart(sock, frames, /*dontwait=*/true) || frames.empty()) {
    return false;
  }
  return parse_proto(frames.back(), msg);
}

bool parse_proto(const zmq::message_t& frame, google::protobuf::MessageLite& msg) {
  return msg.ParseFromArray(frame.data(), static_cast<int>(frame.size()));
}

void fill_header(Header& h, std::uint64_t seq, std::string_view source) {
  h.set_seq(seq);
  h.set_source(std::string(source));
  h.set_send_mono_ns(mono_now_ns());
}

bool parse_header_only(const void* data, std::size_t size, Header& out) {
  using google::protobuf::internal::WireFormatLite;
  google::protobuf::io::CodedInputStream in(static_cast<const std::uint8_t*>(data),
                                            static_cast<int>(size));
  constexpr std::uint32_t kHeaderTag =
      WireFormatLite::MakeTag(1, WireFormatLite::WIRETYPE_LENGTH_DELIMITED);
  while (true) {
    const std::uint32_t tag = in.ReadTag();
    if (tag == 0) {
      return false;  // end of buffer (or malformed) without a header
    }
    if (tag == kHeaderTag) {
      std::uint32_t len = 0;
      const void* ptr = nullptr;
      int avail = 0;
      if (!in.ReadVarint32(&len) || !in.GetDirectBufferPointer(&ptr, &avail) ||
          static_cast<std::uint32_t>(avail) < len) {
        return false;
      }
      return out.ParseFromArray(ptr, static_cast<int>(len));
    }
    if (!WireFormatLite::SkipField(&in, tag)) {
      return false;
    }
  }
}

}  // namespace cell
