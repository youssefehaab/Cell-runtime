#include <benchmark/benchmark.h>

#include <random>
#include <vector>

#include "algo/aabb.hpp"
#include "algo/pallet_packer.hpp"

namespace {

using cell::algo::Aabb;
using cell::algo::Box;
using cell::algo::Pallet;

std::vector<Box> random_boxes(int n) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> side(0.1, 0.5);
  std::uniform_real_distribution<double> weight(1.0, 30.0);
  std::uniform_real_distribution<double> strength(20.0, 200.0);
  std::vector<Box> boxes;
  boxes.reserve(static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    boxes.push_back(Box{i, side(rng), side(rng), side(rng), weight(rng), strength(rng), true});
  }
  return boxes;
}

void BM_AabbIntersects(benchmark::State& state) {
  const Aabb a{0, 0, 0, 1, 1, 1};
  Aabb b{0.5, 0.5, 0.5, 2, 2, 2};
  for (auto _ : state) {
    benchmark::DoNotOptimize(b);
    benchmark::DoNotOptimize(cell::algo::intersects(a, b));
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AabbIntersects)->Name("Aabb/intersects");

void BM_Pack(benchmark::State& state) {
  const auto boxes = random_boxes(static_cast<int>(state.range(0)));
  const Pallet pallet{1.2, 0.8, 1.8, 1500.0};
  double fill = 0.0;
  std::size_t placed = 0;
  for (auto _ : state) {
    auto result = cell::algo::pack(pallet, boxes);
    fill = result.fill_ratio;
    placed = result.placed.size();
    benchmark::DoNotOptimize(result);
  }
  state.counters["placed"] = static_cast<double>(placed);
  state.counters["fill_pct"] = fill * 100.0;
  state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Pack)
    ->Name("PalletPacker/pack")
    ->Arg(10)
    ->Arg(50)
    ->Arg(200)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
