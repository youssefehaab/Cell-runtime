#pragma once

#include <google/protobuf/message_lite.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <zmq.hpp>

#include "cell.pb.h"

namespace cell {

// ---- socket factories -------------------------------------------------------
// All sockets are RAII (zmq::socket_t) and created with LINGER=0 so shutdown
// never hangs on undelivered messages. For ipc:// endpoints the parent
// directory is created before bind.

struct SocketOptions {
  int sndhwm = 1000;  // messages queued per peer before PUB drops / REQ blocks
  int rcvhwm = 1000;
};

zmq::socket_t make_pub(zmq::context_t& ctx, const std::string& bind_endpoint,
                       SocketOptions opts = {});

// SUB connected to `endpoint`, subscribed to each topic prefix ("" = everything).
zmq::socket_t make_sub(zmq::context_t& ctx, const std::string& endpoint,
                       const std::vector<std::string>& topics, SocketOptions opts = {});

zmq::socket_t make_rep(zmq::context_t& ctx, const std::string& bind_endpoint);

// REQ with REQ_RELAXED + REQ_CORRELATE: after a timeout the socket can send a
// new request instead of being stuck, and late replies are discarded.
zmq::socket_t make_req(zmq::context_t& ctx, const std::string& endpoint);

// Create the directory for an ipc:// endpoint (no-op for other transports).
void ensure_ipc_dir(const std::string& endpoint);

// ---- send / receive ---------------------------------------------------------
// All helpers return false instead of throwing on EINTR/EAGAIN so loops can
// re-check shutdown_requested(). Other zmq errors still throw zmq::error_t.

// Two-frame publish: [topic][payload].
bool send_proto(zmq::socket_t& sock, std::string_view topic,
                const google::protobuf::MessageLite& msg);

// Single-frame send (REQ/REP payloads).
bool send_proto(zmq::socket_t& sock, const google::protobuf::MessageLite& msg);

// Wait up to `timeout` for the socket to become readable.
bool poll_in(zmq::socket_t& sock, std::chrono::milliseconds timeout);

// Receive every frame of one message. Non-blocking when `dontwait` is true.
bool recv_multipart(zmq::socket_t& sock, std::vector<zmq::message_t>& frames,
                    bool dontwait = false);

// Receive a single-frame protobuf (REQ/REP). Waits up to `timeout`.
bool recv_proto(zmq::socket_t& sock, google::protobuf::MessageLite& msg,
                std::chrono::milliseconds timeout);

bool parse_proto(const zmq::message_t& frame, google::protobuf::MessageLite& msg);

inline std::string_view frame_view(const zmq::message_t& frame) {
  return {static_cast<const char*>(frame.data()), frame.size()};
}

// ---- headers ----------------------------------------------------------------

// Stamp seq, source and send_mono_ns (= now). Call immediately before sending.
void fill_header(Header& h, std::uint64_t seq, std::string_view source);

// Decode only the Header (field 1) of any cell message without parsing the
// rest of the payload. This is what keeps the broker I/O thread cheap for
// multi-megabyte point-cloud frames.
bool parse_header_only(const void* data, std::size_t size, Header& out);

}  // namespace cell
