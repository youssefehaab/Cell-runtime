#include <benchmark/benchmark.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "common/histogram.hpp"

namespace {

constexpr std::size_t kSampleCount = 1U << 16;  // power of two: cheap index wrap
constexpr std::uint32_t kSeed = 0xC311'0002;
constexpr double kMedianLatencyNs = 50'000.0;
constexpr double kLogSigma = 1.0;

// Latency-shaped input (log-normal around 50 us) generated up front so the
// timed loop measures record(), not the random number generator.
std::vector<std::uint64_t> make_samples() {
  std::mt19937_64 rng(kSeed);
  std::lognormal_distribution<double> dist(std::log(kMedianLatencyNs), kLogSigma);
  std::vector<std::uint64_t> samples(kSampleCount);
  for (auto& v : samples) v = static_cast<std::uint64_t>(dist(rng));
  return samples;
}

void BM_Histogram_Record(benchmark::State& state) {
  const auto samples = make_samples();
  cell::LatencyHistogram hist;
  std::size_t i = 0;
  for (auto _ : state) {
    hist.record(samples[i++ & (kSampleCount - 1)]);
  }
  benchmark::DoNotOptimize(hist.count());
  state.SetItemsProcessed(state.iterations());
}

// Benchmark args are integers, so range(0) is the percentile times ten (999 -> p99.9).
void BM_Histogram_Percentile(benchmark::State& state) {
  constexpr double kArgScale = 10.0;
  const double p = static_cast<double>(state.range(0)) / kArgScale;
  cell::LatencyHistogram hist;
  for (const auto v : make_samples()) hist.record(v);
  for (auto _ : state) {
    benchmark::DoNotOptimize(hist.percentile(p));
  }
}

BENCHMARK(BM_Histogram_Record)->Name("LatencyHistogram/record");
BENCHMARK(BM_Histogram_Percentile)
    ->Name("LatencyHistogram/percentile")
    ->ArgName("p_x10")
    ->Arg(500)
    ->Arg(999);

}  // namespace
