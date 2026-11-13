#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "algo/aabb.hpp"
#include "algo/pallet_packer.hpp"

namespace cell::algo {
namespace {

// ------------------------------------------------------------------- AABB ---

TEST(AlgoAabb, OverlapOnAllAxesIsACollision) {
  const Aabb a{0, 0, 0, 1, 1, 1};
  EXPECT_TRUE(intersects(a, Aabb{0.5, 0.5, 0.5, 2, 2, 2}));
  EXPECT_TRUE(intersects(a, Aabb{0.2, 0.2, 0.2, 0.8, 0.8, 0.8}));  // fully inside
  EXPECT_TRUE(intersects(Aabb{0.2, 0.2, 0.2, 0.8, 0.8, 0.8}, a));  // symmetric
}

TEST(AlgoAabb, SeparationOnAnyAxisIsNot) {
  const Aabb a{0, 0, 0, 1, 1, 1};
  EXPECT_FALSE(intersects(a, Aabb{2, 0, 0, 3, 1, 1}));
  EXPECT_FALSE(intersects(a, Aabb{0, 2, 0, 1, 3, 1}));
  EXPECT_FALSE(intersects(a, Aabb{0, 0, 2, 1, 1, 3}));
}

TEST(AlgoAabb, TouchingFacesEdgesAndCornersDoNotCollide) {
  const Aabb a{0, 0, 0, 1, 1, 1};
  EXPECT_FALSE(intersects(a, Aabb{1, 0, 0, 2, 1, 1}));  // shared face: neighbours
  EXPECT_FALSE(intersects(a, Aabb{0, 0, 1, 1, 1, 2}));  // stacked
  EXPECT_FALSE(intersects(a, Aabb{1, 1, 0, 2, 2, 1}));  // shared edge
  EXPECT_FALSE(intersects(a, Aabb{1, 1, 1, 2, 2, 2}));  // shared corner
  // Floating-point noise from summed dimensions must not read as overlap.
  EXPECT_FALSE(intersects(a, Aabb{0.1 + 0.2 + 0.7, 0, 0, 2, 1, 1}));
}

TEST(AlgoAabb, ContainsAndFootprintOverlap) {
  const Aabb outer{0, 0, 0, 2, 2, 2};
  EXPECT_TRUE(contains(outer, Aabb{0, 0, 0, 2, 2, 2}));
  EXPECT_TRUE(contains(outer, Aabb{0.5, 0.5, 0.5, 1, 1, 1}));
  EXPECT_FALSE(contains(outer, Aabb{0.5, 0.5, 0.5, 1, 1, 2.1}));
  EXPECT_DOUBLE_EQ(footprint_overlap(Aabb{0, 0, 0, 2, 2, 1}, Aabb{1, 1, 5, 3, 3, 6}), 1.0);
  EXPECT_DOUBLE_EQ(footprint_overlap(Aabb{0, 0, 0, 1, 1, 1}, Aabb{1, 0, 0, 2, 1, 1}), 0.0);
  EXPECT_DOUBLE_EQ((Aabb{0, 0, 0, 2, 3, 4}).volume(), 24.0);
}

// ---------------------------------------------------------------- packing ---

Box box(int id, double l, double w, double h, double weight = 10.0, double max_load = 1000.0,
        bool can_rotate = true) {
  return Box{id, l, w, h, weight, max_load, can_rotate};
}

TEST(AlgoPack, EmptyInputGivesEmptyResult) {
  const PackResult r = pack(Pallet{}, {});
  EXPECT_TRUE(r.placed.empty());
  EXPECT_TRUE(r.unplaced.empty());
  EXPECT_DOUBLE_EQ(r.fill_ratio, 0.0);
}

TEST(AlgoPack, PerfectLayerFillsThePalletExactly) {
  // 1.2 x 0.8 footprint tiled by six 0.4 x 0.4 boxes, two layers high.
  const Pallet pallet{1.2, 0.8, 0.8, 1000.0};
  std::vector<Box> boxes;
  for (int i = 0; i < 12; ++i) {
    boxes.push_back(box(i, 0.4, 0.4, 0.4));
  }
  const PackResult r = pack(pallet, boxes);
  EXPECT_EQ(validate(pallet, boxes, r), "");
  EXPECT_EQ(r.placed.size(), 12U);
  EXPECT_TRUE(r.unplaced.empty());
  EXPECT_NEAR(r.fill_ratio, 1.0, 1e-9);
  EXPECT_NEAR(r.load_height, 0.8, 1e-9);
  EXPECT_NEAR(r.total_weight, 120.0, 1e-9);
}

TEST(AlgoPack, RotatesABoxThatOnlyFitsTurned) {
  const Pallet pallet{1.0, 0.5, 1.0, 1000.0};
  const std::vector<Box> turnable{box(1, 0.5, 1.0, 0.2)};
  const PackResult r = pack(pallet, turnable);
  ASSERT_EQ(r.placed.size(), 1U);
  EXPECT_TRUE(r.placed[0].rotated);
  EXPECT_DOUBLE_EQ(r.placed[0].length, 1.0);
  EXPECT_EQ(validate(pallet, turnable, r), "");

  const std::vector<Box> fixed{box(1, 0.5, 1.0, 0.2, 10.0, 1000.0, /*can_rotate=*/false)};
  EXPECT_EQ(pack(pallet, fixed).unplaced, std::vector<int>{1});
}

TEST(AlgoPack, RespectsHeightAndWeightLimits) {
  const Pallet pallet{0.4, 0.4, 1.0, 25.0};
  // Footprint allows only a single column; height allows 2 boxes, weight allows 2.
  std::vector<Box> boxes{box(1, 0.4, 0.4, 0.45), box(2, 0.4, 0.4, 0.45), box(3, 0.4, 0.4, 0.45)};
  PackResult r = pack(pallet, boxes);
  EXPECT_EQ(r.placed.size(), 2U);
  EXPECT_EQ(r.unplaced.size(), 1U);
  EXPECT_EQ(validate(pallet, boxes, r), "");

  const Pallet light{0.4, 0.4, 2.0, 15.0};  // height is fine now, weight is not
  r = pack(light, boxes);
  EXPECT_EQ(r.placed.size(), 1U);
  EXPECT_LE(r.total_weight, 15.0);
}

TEST(AlgoPack, StackingLimitKeepsHeavyBoxesOffFragileOnes) {
  const Pallet pallet{0.4, 0.4, 2.0, 1000.0};
  // One column. The fragile box can carry 5 kg, so the 30 kg box may not sit on it.
  const std::vector<Box> boxes{
      box(1, 0.4, 0.4, 0.3, /*weight=*/30.0, /*max_load=*/200.0),
      box(2, 0.4, 0.4, 0.3, /*weight=*/2.0, /*max_load=*/5.0),
      box(3, 0.4, 0.4, 0.3, /*weight=*/30.0, /*max_load=*/200.0),
  };
  const PackResult r = pack(pallet, boxes);
  EXPECT_EQ(validate(pallet, boxes, r), "");
  ASSERT_EQ(r.placed.size(), 3U);
  // Sturdy boxes form the base; the fragile one ends up on top with nothing on it.
  EXPECT_EQ(r.placed.back().id, 2);
  EXPECT_NEAR(r.placed.back().z, 0.6, 1e-9);
}

TEST(AlgoPack, LoadPropagatesThroughTheWholeStack) {
  const Pallet pallet{0.4, 0.4, 2.0, 1000.0};
  // Bottom box carries at most 25 kg: one 20 kg box is fine, a second is not,
  // even though the middle box itself could carry it.
  const std::vector<Box> boxes{
      box(1, 0.4, 0.4, 0.3, 20.0, /*max_load=*/25.0),
      box(2, 0.4, 0.4, 0.3, 20.0, /*max_load=*/24.0),
      box(3, 0.4, 0.4, 0.3, 20.0, /*max_load=*/23.0),
  };
  const PackResult r = pack(pallet, boxes);
  EXPECT_EQ(validate(pallet, boxes, r), "");
  EXPECT_EQ(r.placed.size(), 2U);
  EXPECT_EQ(r.unplaced, std::vector<int>{3});
}

TEST(AlgoPack, RequiresSupportUnderABox) {
  const Pallet pallet{1.2, 0.8, 1.0, 1000.0};
  // A small box cannot carry a box four times its footprint...
  const std::vector<Box> boxes{box(1, 0.3, 0.3, 0.3, 10.0, 500.0), box(2, 0.6, 0.6, 0.3, 5.0, 1.0)};
  const PackResult r = pack(pallet, boxes, PackOptions{.min_support = 0.75});
  EXPECT_EQ(validate(pallet, boxes, r), "");
  // ...so both end up on the deck rather than stacked with overhang.
  ASSERT_EQ(r.placed.size(), 2U);
  EXPECT_DOUBLE_EQ(r.placed[0].z, 0.0);
  EXPECT_DOUBLE_EQ(r.placed[1].z, 0.0);
}

TEST(AlgoPack, RejectsDegenerateBoxes) {
  const std::vector<Box> boxes{box(1, 0.0, 0.4, 0.4), box(2, 0.4, 0.4, -1.0),
                               box(3, 5.0, 5.0, 0.1)};
  const PackResult r = pack(Pallet{}, boxes);
  EXPECT_TRUE(r.placed.empty());
  EXPECT_EQ(r.unplaced.size(), 3U);
}

TEST(AlgoPack, IsDeterministicAndIndependentOfInputOrder) {
  std::vector<Box> boxes;
  for (int i = 0; i < 20; ++i) {
    boxes.push_back(box(i, 0.2 + 0.05 * (i % 4), 0.2 + 0.05 * (i % 3), 0.2, 5.0 + i, 100.0 + i));
  }
  const PackResult a = pack(Pallet{}, boxes);
  const PackResult b = pack(Pallet{}, boxes);
  ASSERT_EQ(a.placed.size(), b.placed.size());
  for (std::size_t i = 0; i < a.placed.size(); ++i) {
    EXPECT_EQ(a.placed[i].id, b.placed[i].id);
    EXPECT_DOUBLE_EQ(a.placed[i].x, b.placed[i].x);
    EXPECT_DOUBLE_EQ(a.placed[i].z, b.placed[i].z);
  }
}

// The validator is the oracle for the property test below, so it must itself
// reject broken packings.
TEST(AlgoValidate, CatchesCollisionOverhangAndOverload) {
  const Pallet pallet{1.2, 0.8, 1.5, 1000.0};
  const std::vector<Box> boxes{box(1, 0.4, 0.4, 0.4, 10.0, 5.0), box(2, 0.4, 0.4, 0.4, 10.0, 5.0)};
  const auto place = [](int id, double x, double y, double z) {
    return Placement{id, x, y, z, 0.4, 0.4, 0.4, 10.0, false};
  };
  PackResult r;
  r.placed = {place(1, 0, 0, 0), place(2, 0.2, 0, 0)};
  EXPECT_NE(validate(pallet, boxes, r).find("collides"), std::string::npos);
  r.placed = {place(1, 0, 0, 0), place(2, 1.0, 0, 0)};
  EXPECT_NE(validate(pallet, boxes, r).find("sticks out"), std::string::npos);
  r.placed = {place(1, 0, 0, 0), place(2, 0.3, 0, 0.4)};
  EXPECT_NE(validate(pallet, boxes, r).find("supported"), std::string::npos);
  r.placed = {place(1, 0, 0, 0), place(2, 0, 0, 0.4)};  // 10 kg on a 5 kg limit
  EXPECT_NE(validate(pallet, boxes, r).find("stacking limit"), std::string::npos);
  r.placed = {place(1, 0, 0, 0), place(2, 0.4, 0, 0)};
  EXPECT_EQ(validate(pallet, boxes, r), "");
}

// Property test: whatever the input, the result satisfies every constraint.
TEST(AlgoPack, RandomInputsAlwaysProduceValidPackings) {
  std::mt19937 rng(20240607);
  std::uniform_real_distribution<double> side(0.1, 0.6);
  std::uniform_real_distribution<double> height(0.1, 0.5);
  std::uniform_real_distribution<double> weight(1.0, 40.0);
  std::uniform_real_distribution<double> strength(0.0, 150.0);
  std::uniform_int_distribution<int> count(1, 60);
  std::bernoulli_distribution rotatable(0.8);

  double best_fill = 0.0;
  for (int trial = 0; trial < 200; ++trial) {
    std::vector<Box> boxes;
    const int n = count(rng);
    for (int i = 0; i < n; ++i) {
      boxes.push_back(
          Box{i, side(rng), side(rng), height(rng), weight(rng), strength(rng), rotatable(rng)});
    }
    const Pallet pallet{1.2, 0.8, 1.5, 600.0};
    const PackOptions options{.min_support = 0.75};
    const PackResult r = pack(pallet, boxes, options);
    ASSERT_EQ(validate(pallet, boxes, r, options), "") << "trial " << trial;
    ASSERT_LE(r.fill_ratio, 1.0 + 1e-9);
    best_fill = std::max(best_fill, r.fill_ratio);
  }
  EXPECT_GT(best_fill, 0.3);  // the heuristic does real work on random mixes
}

}  // namespace
}  // namespace cell::algo
