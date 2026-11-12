#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "algo/aabb.hpp"

namespace cell::algo {

// A box to be palletised. Dimensions in metres, weights in kilograms.
struct Box {
  int id = 0;
  double length = 0.0;  // along X before rotation
  double width = 0.0;   // along Y before rotation
  double height = 0.0;  // along Z; boxes are never tipped on their side
  double weight = 0.0;
  // Stacking limit: the total weight this box can carry on top of it.
  double max_load = 0.0;
  bool can_rotate = true;  // may be turned 90 degrees about the vertical axis
};

struct Pallet {
  double length = 1.2;  // EUR pallet footprint by default
  double width = 0.8;
  double max_height = 1.5;  // of the load, above the deck
  double max_weight = 1000.0;
};

struct PackOptions {
  // Fraction of a box's footprint that must rest on the boxes below it
  // (or on the deck). 1.0 forbids any overhang.
  double min_support = 0.75;
};

struct Placement {
  int id = 0;                        // Box::id
  double x = 0.0, y = 0.0, z = 0.0;  // minimum corner
  double length = 0.0, width = 0.0;  // footprint as placed (swapped if rotated)
  double height = 0.0;
  double weight = 0.0;
  bool rotated = false;

  Aabb bounds() const noexcept { return {x, y, z, x + length, y + width, z + height}; }
};

struct PackResult {
  std::vector<Placement> placed;  // in placement order
  std::vector<int> unplaced;      // ids that did not fit
  double fill_ratio = 0.0;        // placed volume / (pallet footprint * max_height)
  double total_weight = 0.0;
  double load_height = 0.0;  // top of the highest box
};

// Greedy pallet packing.
//
// Order:    boxes are sorted sturdiest-first: larger stacking limit, then larger
//           footprint, then heavier - so strong, wide boxes form the base.
// Position: each box takes the lowest feasible "extreme point" (corner created
//           by earlier boxes), ties broken toward the back-left corner, trying
//           both footprint orientations when allowed.
// Feasible: inside the pallet volume, no AABB collision, at least
//           `min_support` of the footprint supported, pallet weight limit not
//           exceeded, and no box below pushed past its max_load - the new
//           box's weight is propagated down through every box that carries it,
//           split in proportion to contact area.
//
// A heuristic, not an optimal solver (3D bin packing is NP-hard). It runs in
// O(n^2 * p) for n boxes and p candidate points and is deterministic.
PackResult pack(const Pallet& pallet, std::vector<Box> boxes, const PackOptions& options = {});

// Independent check of a result against every constraint above. Returns an
// empty string if the packing is valid, otherwise a description of the first
// violation. Used by the tests as an oracle that shares no code with pack().
std::string validate(const Pallet& pallet, const std::vector<Box>& boxes, const PackResult& result,
                     const PackOptions& options = {});

}  // namespace cell::algo
