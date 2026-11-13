#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <zmq.hpp>

#include "broker/broker_config.hpp"
#include "common/spsc_ring.hpp"

namespace cell::broker {

enum class ItemKind : std::uint8_t { kData, kHeartbeat };

// What the I/O thread hands to the processing thread. The payload frame is
// moved in (zmq_msg_move: a 64-byte struct, the bytes themselves are never
// copied) and released by the processing thread; zmq message buffers are
// reference-counted atomically, so freeing on another thread is safe.
struct RingItem {
  zmq::message_t payload;
  std::uint64_t seq = 0;
  std::uint64_t send_ns = 0;  // Header.send_mono_ns
  std::uint64_t recv_ns = 0;  // CLOCK_MONOTONIC when the I/O thread dequeued it
  std::uint32_t size = 0;     // payload bytes
  std::uint16_t stream = 0;   // index into BrokerConfig::streams
  ItemKind kind = ItemKind::kData;
};

// Per-thread start-up and liveness flags, on their own cache line so the
// counter one thread bumps every iteration does not bounce another's.
struct alignas(kCacheLineSize) ThreadState {
  std::atomic<std::uint64_t> beats{0};  // incremented once per loop iteration
  std::atomic<bool> ready{false};       // sockets created and bound/connected
  std::atomic<bool> done{false};        // thread function returned
};

// A counter written by exactly one thread and read by others.
struct alignas(kCacheLineSize) Counter {
  std::atomic<std::uint64_t> value{0};

  void add(std::uint64_t n = 1) noexcept {
    // Single writer: load + store avoids the lock prefix of fetch_add.
    value.store(value.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
  }
  std::uint64_t get() const noexcept { return value.load(std::memory_order_relaxed); }
};

// Everything the broker threads share. The zmq context is thread-safe; sockets
// are not, and none appears here: each thread creates, uses and closes its own.
//
//   I/O thread         writes: ring (producer), drops[], hb_drops, malformed
//   processing thread  reads those; ring (consumer)
//   command thread     touches nothing shared except its ThreadState
struct Shared {
  Shared(const BrokerConfig& config, zmq::context_t& context)
      : cfg(config),
        ctx(context),
        ring(config.ring_capacity),
        drops(std::make_unique<Counter[]>(config.streams.size())) {}

  const BrokerConfig& cfg;
  zmq::context_t& ctx;
  SpscRing<RingItem> ring;
  std::unique_ptr<Counter[]> drops;  // per stream: data messages dropped, ring full
  Counter hb_drops;                  // heartbeats dropped, ring full
  Counter malformed;                 // wrong frame count, unknown topic or no header

  ThreadState io;
  ThreadState processing;
  ThreadState command;
  std::atomic<bool> failed{false};  // a thread died with an exception
};

// Thread bodies. Each returns when cell::shutdown_requested() and throws on
// unrecoverable errors (bad endpoint, zmq failure).
void run_io_thread(Shared& shared);
void run_processing_thread(Shared& shared);
void run_command_thread(Shared& shared);

}  // namespace cell::broker
