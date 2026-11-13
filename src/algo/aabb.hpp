#pragma once

#include <algorithm>

namespace cell::algo {

// Axis-aligned bounding box, half-open in spirit: boxes that only share a
// face, edge or corner do NOT intersect, so neighbours on a pallet and a box
// resting on another are not collisions.
struct Aabb {
  double min_x = 0.0, min_y = 0.0, min_z = 0.0;
  double max_x = 0.0, max_y = 0.0, max_z = 0.0;

  constexpr double volume() const noexcept {
    return (max_x - min_x) * (max_y - min_y) * (max_z - min_z);
  }
};

// Tolerance for geometric comparisons (metres): absorbs floating-point noise
// from summing box dimensions without letting real overlaps through.
inline constexpr double kEpsilon = 1e-9;

// True if the interiors overlap by more than kEpsilon on all three axes.
constexpr bool intersects(const Aabb& a, const Aabb& b) noexcept {
  return a.min_x < b.max_x - kEpsilon && b.min_x < a.max_x - kEpsilon &&
         a.min_y < b.max_y - kEpsilon && b.min_y < a.max_y - kEpsilon &&
         a.min_z < b.max_z - kEpsilon && b.min_z < a.max_z - kEpsilon;
}

// True if `inner` lies entirely within `outer` (touching the boundary is fine).
constexpr bool contains(const Aabb& outer, const Aabb& inner) noexcept {
  return inner.min_x >= outer.min_x - kEpsilon && inner.max_x <= outer.max_x + kEpsilon &&
         inner.min_y >= outer.min_y - kEpsilon && inner.max_y <= outer.max_y + kEpsilon &&
         inner.min_z >= outer.min_z - kEpsilon && inner.max_z <= outer.max_z + kEpsilon;
}

// Area of the overlap of the two boxes' footprints in the XY plane (>= 0).
constexpr double footprint_overlap(const Aabb& a, const Aabb& b) noexcept {
  const double dx = std::min(a.max_x, b.max_x) - std::max(a.min_x, b.min_x);
  const double dy = std::min(a.max_y, b.max_y) - std::max(a.min_y, b.min_y);
  return dx > 0.0 && dy > 0.0 ? dx * dy : 0.0;
}

}  // namespace cell::algo
