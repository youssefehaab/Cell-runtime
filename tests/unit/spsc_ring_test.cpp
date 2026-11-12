#include "common/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(__SANITIZE_THREAD__)
#define CELL_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define CELL_TSAN 1
#endif
#endif

namespace cell {
namespace {

static_assert(!std::is_copy_constructible_v<SpscRing<int>>);
static_assert(!std::is_move_constructible_v<SpscRing<int>>);
static_assert(!std::is_copy_assignable_v<SpscRing<int>>);
static_assert(!std::is_move_assignable_v<SpscRing<int>>);
static_assert(alignof(SpscRing<int>) == kCacheLineSize);
static_assert(sizeof(SpscRing<int>) % kCacheLineSize == 0);
static_assert(noexcept(std::declval<SpscRing<int>&>().try_push(1)));
static_assert(noexcept(std::declval<SpscRing<int>&>().try_pop(std::declval<int&>())));
static_assert(
    !noexcept(std::declval<SpscRing<std::string>&>().try_push(std::declval<const std::string&>())));

// Counts live instances and total constructions/destructions so tests can
// prove every element is destroyed exactly once.
struct Tracked {
  static inline int live = 0;
  static inline int constructed = 0;
  static inline int destroyed = 0;
  static void reset_counters() { live = constructed = destroyed = 0; }

  int value = 0;

  Tracked() { birth(); }
  explicit Tracked(int v) : value(v) { birth(); }
  Tracked(const Tracked& o) : value(o.value) { birth(); }
  Tracked(Tracked&& o) noexcept : value(std::exchange(o.value, -1)) { birth(); }
  Tracked& operator=(const Tracked&) = default;
  Tracked& operator=(Tracked&& o) noexcept {
    value = std::exchange(o.value, -1);
    return *this;
  }
  ~Tracked() {
    --live;
    ++destroyed;
  }

 private:
  static void birth() {
    ++live;
    ++constructed;
  }
};

TEST(SpscRing, CapacityRoundsUpToPowerOfTwo) {
  EXPECT_EQ(SpscRing<int>(0).capacity(), 2U);
  EXPECT_EQ(SpscRing<int>(1).capacity(), 2U);
  EXPECT_EQ(SpscRing<int>(2).capacity(), 2U);
  EXPECT_EQ(SpscRing<int>(3).capacity(), 4U);
  EXPECT_EQ(SpscRing<int>(1000).capacity(), 1024U);
  EXPECT_EQ(SpscRing<int>(1024).capacity(), 1024U);
  EXPECT_EQ(SpscRing<int>(1025).capacity(), 2048U);
  EXPECT_THROW(SpscRing<int>(SIZE_MAX), std::length_error);
}

TEST(SpscRing, EmptyAndFull) {
  SpscRing<int> ring(4);
  int out = -1;
  EXPECT_TRUE(ring.empty_approx());
  EXPECT_EQ(ring.size_approx(), 0U);
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_EQ(out, -1);  // untouched on failure

  // Every one of the capacity() slots is usable.
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.try_push(i));
    EXPECT_EQ(ring.size_approx(), static_cast<std::size_t>(i) + 1);
  }
  EXPECT_FALSE(ring.try_push(99));
  EXPECT_FALSE(ring.try_emplace(99));
  EXPECT_EQ(ring.size_approx(), ring.capacity());
  EXPECT_FALSE(ring.empty_approx());

  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out, i);
  }
  EXPECT_FALSE(ring.try_pop(out));
  EXPECT_TRUE(ring.empty_approx());

  // A slot freed by the consumer is immediately reusable by the producer.
  EXPECT_TRUE(ring.try_push(7));
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, 7);
}

TEST(SpscRing, FifoOrderAcrossWraparound) {
  constexpr int kCycles = 10'000;
  SpscRing<int> ring(8);
  int next_push = 0;
  int next_pop = 0;
  // Varying burst sizes walk the indices through every phase of the mask.
  for (int cycle = 0; cycle < kCycles; ++cycle) {
    const int burst = 1 + cycle % static_cast<int>(ring.capacity());
    for (int i = 0; i < burst; ++i) {
      ASSERT_TRUE(ring.try_push(next_push++));
    }
    for (int i = 0; i < burst; ++i) {
      int out = -1;
      ASSERT_TRUE(ring.try_pop(out));
      ASSERT_EQ(out, next_pop++);
    }
    ASSERT_TRUE(ring.empty_approx());
  }
  EXPECT_EQ(next_push, next_pop);
}

TEST(SpscRing, MoveOnlyType) {
  SpscRing<std::unique_ptr<int>> ring(2);
  auto a = std::make_unique<int>(1);
  EXPECT_TRUE(ring.try_push(std::move(a)));
  EXPECT_EQ(a, nullptr);
  EXPECT_TRUE(ring.try_emplace(std::make_unique<int>(2)));

  // A rejected push must leave the argument intact so the caller can retry or drop it.
  auto c = std::make_unique<int>(3);
  EXPECT_FALSE(ring.try_push(std::move(c)));
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(*c, 3);

  std::unique_ptr<int> out;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(*out, 1);
  EXPECT_TRUE(ring.try_push(std::move(c)));
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(*out, 2);
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(*out, 3);
  EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, CopyPushAndEmplace) {
  SpscRing<std::string> ring(4);
  const std::string s = "copied";
  EXPECT_TRUE(ring.try_push(s));
  EXPECT_EQ(s, "copied");
  EXPECT_TRUE(ring.try_emplace(3, 'x'));
  std::string out;
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, "copied");
  ASSERT_TRUE(ring.try_pop(out));
  EXPECT_EQ(out, "xxx");
}

TEST(SpscRing, PopDestroysElement) {
  Tracked::reset_counters();
  {
    SpscRing<Tracked> ring(4);
    EXPECT_EQ(Tracked::constructed, 0);  // empty slots hold no objects

    ASSERT_TRUE(ring.try_emplace(1));
    ASSERT_TRUE(ring.try_emplace(2));
    EXPECT_EQ(Tracked::live, 2);

    Tracked out;
    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.value, 1);
    EXPECT_EQ(Tracked::live, 2);  // `out` + one still queued

    ASSERT_TRUE(ring.try_pop(out));
    EXPECT_EQ(out.value, 2);
    EXPECT_EQ(Tracked::live, 1);  // only `out`
  }
  EXPECT_EQ(Tracked::live, 0);
  EXPECT_EQ(Tracked::constructed, Tracked::destroyed);
}

TEST(SpscRing, FailedPushConstructsNothing) {
  Tracked::reset_counters();
  SpscRing<Tracked> ring(2);
  ASSERT_TRUE(ring.try_emplace(1));
  ASSERT_TRUE(ring.try_emplace(2));
  const int constructed_before = Tracked::constructed;
  EXPECT_FALSE(ring.try_emplace(3));
  EXPECT_EQ(Tracked::constructed, constructed_before);
}

TEST(SpscRing, DestructorDestroysLeftovers) {
  Tracked::reset_counters();
  {
    SpscRing<Tracked> ring(8);
    // Advance past one full lap first so the leftovers straddle the wrap point.
    for (int i = 0; i < 6; ++i) ASSERT_TRUE(ring.try_emplace(i));
    {
      Tracked out;
      for (int i = 0; i < 6; ++i) ASSERT_TRUE(ring.try_pop(out));
    }
    EXPECT_EQ(Tracked::live, 0);
    for (int i = 0; i < 5; ++i) ASSERT_TRUE(ring.try_emplace(i));
    EXPECT_EQ(Tracked::live, 5);
  }
  EXPECT_EQ(Tracked::live, 0);
  EXPECT_EQ(Tracked::constructed, Tracked::destroyed);
}

// One producer, one consumer, a ring far smaller than the item count so both
// the full and the empty path are exercised constantly. Run under TSan in CI.
TEST(SpscRing, TwoThreadStress) {
#if defined(CELL_TSAN)
  constexpr std::uint64_t kItems = 1'000'000;  // TSan slows atomics ~10x
#else
  constexpr std::uint64_t kItems = 2'000'000;
#endif
  constexpr std::size_t kCapacity = 1024;

  SpscRing<std::uint64_t> ring(kCapacity);
  std::uint64_t pushed_sum = 0;
  std::uint64_t popped_sum = 0;
  std::uint64_t popped = 0;
  std::uint64_t first_mismatch = kItems;  // kItems == none
  std::atomic<bool> start{false};

  std::thread producer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring.try_push(i)) std::this_thread::yield();
      pushed_sum += i;
    }
  });
  std::thread consumer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    std::uint64_t v = 0;
    while (popped < kItems) {
      if (!ring.try_pop(v)) {
        std::this_thread::yield();
        continue;
      }
      if (v != popped && first_mismatch == kItems) first_mismatch = popped;
      popped_sum += v;
      ++popped;
    }
  });
  start.store(true, std::memory_order_release);
  producer.join();
  consumer.join();

  EXPECT_EQ(first_mismatch, kItems) << "out-of-order value at position " << first_mismatch;
  EXPECT_EQ(popped, kItems);
  EXPECT_EQ(popped_sum, pushed_sum);
  EXPECT_EQ(popped_sum, kItems * (kItems - 1) / 2);
  EXPECT_TRUE(ring.empty_approx());
}

// Same shape with a move-only heap-owning payload: a lost or duplicated slot
// shows up as a leak / double free under ASan, a racy slot reuse under TSan.
TEST(SpscRing, TwoThreadStressMoveOnly) {
#if defined(CELL_TSAN)
  constexpr int kItems = 50'000;
#else
  constexpr int kItems = 300'000;
#endif
  SpscRing<std::unique_ptr<int>> ring(64);
  bool in_order = true;

  std::thread producer([&] {
    for (int i = 0; i < kItems; ++i) {
      auto item = std::make_unique<int>(i);
      while (!ring.try_push(std::move(item))) std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    std::unique_ptr<int> out;
    for (int expected = 0; expected < kItems;) {
      if (!ring.try_pop(out)) {
        std::this_thread::yield();
        continue;
      }
      in_order = in_order && out != nullptr && *out == expected;
      ++expected;
    }
  });
  producer.join();
  consumer.join();
  EXPECT_TRUE(in_order);
  EXPECT_TRUE(ring.empty_approx());
}

}  // namespace
}  // namespace cell
