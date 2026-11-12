#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "common/spsc_ring.hpp"

namespace {

constexpr std::size_t kQueueCapacity = 1024;

// Baseline with the same bounded, non-blocking interface as cell::SpscRing so
// the benchmarks below can drive either queue unchanged.
template <typename T>
class MutexDequeQueue {
 public:
  explicit MutexDequeQueue(std::size_t capacity) : capacity_(capacity) {}

  bool try_push(T v) {
    const std::lock_guard lock(mutex_);
    if (items_.size() >= capacity_) return false;
    items_.push_back(std::move(v));
    return true;
  }

  bool try_pop(T& out) {
    const std::lock_guard lock(mutex_);
    if (items_.empty()) return false;
    out = std::move(items_.front());
    items_.pop_front();
    return true;
  }

 private:
  const std::size_t capacity_;
  std::mutex mutex_;
  std::deque<T> items_;
};

// Uncontended cost of one push + one pop on a single thread.
template <typename Queue>
void BM_PushPop_SingleThread(benchmark::State& state) {
  Queue queue(kQueueCapacity);
  std::uint64_t value = 0;
  std::uint64_t out = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(queue.try_push(value++));
    benchmark::DoNotOptimize(queue.try_pop(out));
    benchmark::DoNotOptimize(out);
  }
  state.SetItemsProcessed(state.iterations());
}

// Throughput with this thread producing and a second thread consuming. Both
// sides yield when the queue is full/empty, which is how the broker threads
// would back off, and keeps the numbers meaningful on a busy machine.
template <typename Queue>
void BM_Throughput_TwoThreads(benchmark::State& state) {
  Queue queue(kQueueCapacity);
  std::atomic<bool> producer_done{false};
  std::uint64_t consumed = 0;

  std::thread consumer([&] {
    std::uint64_t out = 0;
    for (;;) {
      if (queue.try_pop(out)) {
        ++consumed;
        continue;
      }
      // The flag is read only after a failed pop and followed by a final
      // drain: everything pushed before the flag was set is visible to it.
      if (producer_done.load(std::memory_order_acquire)) {
        while (queue.try_pop(out)) ++consumed;
        return;
      }
      std::this_thread::yield();
    }
  });

  std::uint64_t produced = 0;
  for (auto _ : state) {
    while (!queue.try_push(produced)) std::this_thread::yield();
    ++produced;
  }
  producer_done.store(true, std::memory_order_release);
  consumer.join();

  if (consumed != produced) state.SkipWithError("consumer did not receive every item");
  state.SetItemsProcessed(static_cast<std::int64_t>(consumed));
}

using SpscU64 = cell::SpscRing<std::uint64_t>;
using MutexDequeU64 = MutexDequeQueue<std::uint64_t>;

BENCHMARK(BM_PushPop_SingleThread<SpscU64>)->Name("SpscRing/PushPop/1thread");
BENCHMARK(BM_PushPop_SingleThread<MutexDequeU64>)->Name("MutexDeque/PushPop/1thread");
BENCHMARK(BM_Throughput_TwoThreads<SpscU64>)->Name("SpscRing/Throughput/2threads")->UseRealTime();
BENCHMARK(BM_Throughput_TwoThreads<MutexDequeU64>)
    ->Name("MutexDeque/Throughput/2threads")
    ->UseRealTime();

}  // namespace
