#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cell {

// Fixed instead of std::hardware_destructive_interference_size: that constant
// varies with -mtune (GCC warns about exactly this via -Winterference-size when
// it appears in a header, and Clang 18 + libstdc++ does not define it at all),
// so using it would make the class layout depend on compiler and flags. 64
// bytes is the line size on every x86-64 and most AArch64 parts we target.
inline constexpr std::size_t kCacheLineSize = 64;

// Bounded lock-free single-producer / single-consumer FIFO.
//
// Threading contract: at most one thread calls try_push/try_emplace (the
// producer) and at most one calls try_pop (the consumer). size_approx(),
// empty_approx() and capacity() may be called from any thread. Construction
// and destruction must not race with any other call.
//
// Indices are 64-bit counters that only ever increase and are masked on slot
// access, so "full" is `tail - head == capacity` and "empty" is `tail == head`.
// No slot is sacrificed to tell the two apart: all capacity() slots are usable.
// (At 1e9 ops/s a 64-bit counter wraps after ~584 years.)
//
// Both operations are wait-free: no loops, no CAS, no syscalls.
template <typename T>
class SpscRing {
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

 public:
  // Capacity is `min_capacity` rounded up to a power of two, and at least 2.
  explicit SpscRing(std::size_t min_capacity)
      : capacity_(round_up_capacity(min_capacity)),
        mask_(capacity_ - 1),
        slots_(std::make_unique_for_overwrite<Slot[]>(capacity_)) {}

  ~SpscRing() {
    // No concurrent access by contract, so plain relaxed reads are enough.
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    for (std::uint64_t i = head_.load(std::memory_order_relaxed); i != tail; ++i) {
      std::destroy_at(slot(i));
    }
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  // Producer only. Constructs a T in place from `args`. Returns false when the
  // ring is full, in which case `args` are not touched (nothing is moved from).
  // If T's constructor throws, the ring is unchanged.
  template <typename... A>
  bool try_emplace(A&&... args) noexcept(std::is_nothrow_constructible_v<T, A&&...>) {
    // relaxed: tail_ is written only by this thread.
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == capacity_) {
      // Looks full against the stale snapshot; refresh it. acquire pairs with
      // the consumer's release store of head_, so the consumer's destruction
      // of the slot we are about to reuse happens-before our construction.
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == capacity_) {
        return false;
      }
    }
    std::construct_at(slot(tail), std::forward<A>(args)...);
    // release: publishes the constructed element to the consumer's acquire load.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool try_push(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
    return try_emplace(std::move(v));
  }

  bool try_push(const T& v) noexcept(std::is_nothrow_copy_constructible_v<T>) {
    return try_emplace(v);
  }

  // Consumer only. Move-assigns the oldest element into `out` and destroys the
  // slot. Returns false when the ring is empty (`out` untouched). If the move
  // assignment throws, the element stays at the front of the queue.
  bool try_pop(T& out) noexcept(std::is_nothrow_move_assignable_v<T>) {
    // relaxed: head_ is written only by this thread.
    const std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      // acquire pairs with the producer's release store of tail_, making the
      // element's construction visible before we read it.
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) {
        return false;
      }
    }
    T* const elem = slot(head);
    out = std::move(*elem);
    std::destroy_at(elem);
    // release: hands the now-empty slot back to the producer's acquire load.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Number of usable slots (a power of two).
  std::size_t capacity() const noexcept { return capacity_; }

  // Snapshot for monitoring; exact only when both sides are quiescent.
  std::size_t size_approx() const noexcept {
    // head is read first: tail never lags a previously read head, so the
    // difference cannot underflow. It can exceed capacity if both sides made
    // progress between the two loads, hence the clamp. Nothing is dereferenced
    // on the strength of these values, so relaxed would also be correct;
    // acquire just keeps the snapshot as fresh as the other loads.
    const std::uint64_t head = head_.load(std::memory_order_acquire);
    const std::uint64_t tail = tail_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::min<std::uint64_t>(tail - head, capacity_));
  }

  bool empty_approx() const noexcept { return size_approx() == 0; }

 private:
  // Raw, correctly aligned storage; T lifetimes are managed explicitly so that
  // empty slots hold no object and T need not be default-constructible.
  struct Slot {
    alignas(T) std::byte bytes[sizeof(T)];
  };

  static constexpr std::size_t kMinCapacity = 2;
  static constexpr std::size_t kMaxCapacity = std::size_t{1}
                                              << (std::numeric_limits<std::size_t>::digits - 1);

  static std::size_t round_up_capacity(std::size_t min_capacity) {
    if (min_capacity > kMaxCapacity) {
      throw std::length_error("SpscRing: capacity too large");
    }
    return std::bit_ceil(std::max(min_capacity, kMinCapacity));
  }

  T* slot(std::uint64_t index) noexcept {
    return std::launder(reinterpret_cast<T*>(slots_[index & mask_].bytes));
  }

  // Immutable after construction; read by both threads, so kept off the two
  // write-hot lines below.
  const std::size_t capacity_;
  const std::uint64_t mask_;
  const std::unique_ptr<Slot[]> slots_;

  // Producer-owned line: tail_ plus the producer's private snapshot of head_.
  // The snapshot means the consumer's line is only read when the ring looks
  // full, not on every push.
  alignas(kCacheLineSize) std::atomic<std::uint64_t> tail_{0};
  std::uint64_t head_cache_ = 0;

  // Consumer-owned line: head_ plus the consumer's private snapshot of tail_.
  // alignas on the members also pads sizeof(SpscRing) to a whole line, so a
  // neighbouring object cannot share this one.
  alignas(kCacheLineSize) std::atomic<std::uint64_t> head_{0};
  std::uint64_t tail_cache_ = 0;
};

}  // namespace cell
