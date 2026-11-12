#include "algo/pallet_packer.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace cell::algo {
namespace {

struct Point {
  double x, y, z;
};

// One box resting on another: `below` carries `share` of the upper box's load.
struct Support {
  std::size_t below;
  double share;
};

struct Placed {
  Placement placement;
  double max_load;
  double load = 0.0;  // weight currently carried on top
  std::vector<Support> supports;
};

bool rests_on(const Aabb& upper, const Aabb& lower) noexcept {
  return std::abs(upper.min_z - lower.max_z) <= kEpsilon;
}

// Who carries a box at `bounds`, and with what share of its weight. Returns the
// supported fraction of the footprint (1.0 on the deck).
double find_supports(const std::vector<Placed>& placed, const Aabb& bounds,
                     std::vector<Support>& out) {
  out.clear();
  if (bounds.min_z <= kEpsilon) {
    return 1.0;
  }
  const double footprint = (bounds.max_x - bounds.min_x) * (bounds.max_y - bounds.min_y);
  double supported = 0.0;
  for (std::size_t i = 0; i < placed.size(); ++i) {
    const Aabb lower = placed[i].placement.bounds();
    if (!rests_on(bounds, lower)) {
      continue;
    }
    const double area = footprint_overlap(bounds, lower);
    if (area > kEpsilon) {
      out.push_back({i, area});
      supported += area;
    }
  }
  for (auto& s : out) {
    s.share /= supported;  // the supporters share the whole weight between them
  }
  return supported / footprint;
}

// Add `weight` on top of each supporter and everything beneath it.
void propagate(const std::vector<Placed>& placed, const std::vector<Support>& supports,
               double weight, std::map<std::size_t, double>& added) {
  for (const auto& s : supports) {
    const double part = weight * s.share;
    added[s.below] += part;
    propagate(placed, placed[s.below].supports, part, added);
  }
}

bool load_ok(const std::vector<Placed>& placed, const std::map<std::size_t, double>& added) {
  return std::all_of(added.begin(), added.end(), [&](const auto& entry) {
    const Placed& p = placed[entry.first];
    return p.load + entry.second <= p.max_load + kEpsilon;
  });
}

void add_point(std::vector<Point>& points, Point p) {
  const bool known = std::any_of(points.begin(), points.end(), [&](const Point& q) {
    return std::abs(q.x - p.x) <= kEpsilon && std::abs(q.y - p.y) <= kEpsilon &&
           std::abs(q.z - p.z) <= kEpsilon;
  });
  if (!known) {
    points.push_back(p);
  }
}

}  // namespace

PackResult pack(const Pallet& pallet, std::vector<Box> boxes, const PackOptions& options) {
  std::stable_sort(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
    if (a.max_load != b.max_load) {
      return a.max_load > b.max_load;
    }
    const double area_a = a.length * a.width;
    const double area_b = b.length * b.width;
    if (area_a != area_b) {
      return area_a > area_b;
    }
    return a.weight > b.weight;
  });

  const Aabb volume{0.0, 0.0, 0.0, pallet.length, pallet.width, pallet.max_height};
  std::vector<Placed> placed;
  std::vector<Point> points{{0.0, 0.0, 0.0}};
  PackResult result;
  std::vector<Support> supports;
  std::map<std::size_t, double> added;

  for (const Box& box : boxes) {
    const bool sane = box.length > 0.0 && box.width > 0.0 && box.height > 0.0 && box.weight >= 0.0;
    if (!sane || result.total_weight + box.weight > pallet.max_weight + kEpsilon) {
      result.unplaced.push_back(box.id);
      continue;
    }

    // Lowest point first, then toward the back-left corner: fills each layer
    // before starting the next and keeps the load compact.
    std::sort(points.begin(), points.end(), [](const Point& a, const Point& b) {
      if (a.z != b.z) return a.z < b.z;
      if (a.y != b.y) return a.y < b.y;
      return a.x < b.x;
    });

    bool done = false;
    const int orientations = box.can_rotate && box.length != box.width ? 2 : 1;
    for (std::size_t pi = 0; pi < points.size() && !done; ++pi) {
      for (int o = 0; o < orientations && !done; ++o) {
        Placement cand;
        cand.id = box.id;
        cand.x = points[pi].x;
        cand.y = points[pi].y;
        cand.z = points[pi].z;
        cand.length = o == 0 ? box.length : box.width;
        cand.width = o == 0 ? box.width : box.length;
        cand.height = box.height;
        cand.weight = box.weight;
        cand.rotated = o == 1;
        const Aabb bounds = cand.bounds();

        if (!contains(volume, bounds)) {
          continue;
        }
        const bool collides = std::any_of(placed.begin(), placed.end(), [&](const Placed& p) {
          return intersects(bounds, p.placement.bounds());
        });
        if (collides || find_supports(placed, bounds, supports) < options.min_support - kEpsilon) {
          continue;
        }
        added.clear();
        propagate(placed, supports, box.weight, added);
        if (!load_ok(placed, added)) {
          continue;
        }

        for (const auto& [index, extra] : added) {
          placed[index].load += extra;
        }
        placed.push_back({cand, box.max_load, 0.0, supports});
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(pi));
        add_point(points, {bounds.max_x, bounds.min_y, bounds.min_z});
        add_point(points, {bounds.min_x, bounds.max_y, bounds.min_z});
        add_point(points, {bounds.min_x, bounds.min_y, bounds.max_z});
        result.total_weight += box.weight;
        result.load_height = std::max(result.load_height, bounds.max_z);
        done = true;
      }
    }
    if (!done) {
      result.unplaced.push_back(box.id);
    }
  }

  double used = 0.0;
  result.placed.reserve(placed.size());
  for (const Placed& p : placed) {
    used += p.placement.bounds().volume();
    result.placed.push_back(p.placement);
  }
  const double capacity = volume.volume();
  result.fill_ratio = capacity > 0.0 ? used / capacity : 0.0;
  return result;
}

std::string validate(const Pallet& pallet, const std::vector<Box>& boxes, const PackResult& result,
                     const PackOptions& options) {
  const auto describe = [](const Placement& p) { return "box " + std::to_string(p.id); };
  const Aabb volume{0.0, 0.0, 0.0, pallet.length, pallet.width, pallet.max_height};

  std::map<int, const Box*> by_id;
  for (const Box& b : boxes) {
    by_id[b.id] = &b;
  }
  if (result.placed.size() + result.unplaced.size() != boxes.size()) {
    return "placed + unplaced does not account for every box";
  }

  const std::size_t n = result.placed.size();
  double total_weight = 0.0;
  std::map<int, bool> seen;
  for (std::size_t i = 0; i < n; ++i) {
    const Placement& p = result.placed[i];
    const auto it = by_id.find(p.id);
    if (it == by_id.end() || seen[p.id]) {
      return describe(p) + " is unknown or placed twice";
    }
    seen[p.id] = true;
    const Box& b = *it->second;
    const bool as_is = p.length == b.length && p.width == b.width;
    const bool turned = b.can_rotate && p.length == b.width && p.width == b.length;
    if (p.height != b.height || !(as_is || turned)) {
      return describe(p) + " has dimensions that match no allowed orientation";
    }
    if (!contains(volume, p.bounds())) {
      return describe(p) + " sticks out of the pallet volume";
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (intersects(p.bounds(), result.placed[j].bounds())) {
        return describe(p) + " collides with " + describe(result.placed[j]);
      }
    }
    total_weight += b.weight;
  }
  if (total_weight > pallet.max_weight + 1e-6) {
    return "pallet weight limit exceeded";
  }

  // Support and stacking load, computed top-down: process boxes from the
  // highest base to the lowest so each box's carried load is final before it
  // is passed on to the boxes beneath it.
  std::vector<std::size_t> order(n);
  for (std::size_t i = 0; i < n; ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(),
            [&](std::size_t a, std::size_t b) { return result.placed[a].z > result.placed[b].z; });
  std::vector<double> carried(n, 0.0);
  for (const std::size_t i : order) {
    const Placement& p = result.placed[i];
    const Box& b = *by_id[p.id];
    if (carried[i] > b.max_load + 1e-6) {
      return describe(p) + " carries more than its stacking limit";
    }
    if (p.z <= kEpsilon) {
      continue;  // on the deck
    }
    const Aabb upper = p.bounds();
    double supported = 0.0;
    std::vector<std::pair<std::size_t, double>> under;
    for (std::size_t j = 0; j < n; ++j) {
      const Aabb lower = result.placed[j].bounds();
      if (j == i || std::abs(upper.min_z - lower.max_z) > kEpsilon) {
        continue;
      }
      const double area = footprint_overlap(upper, lower);
      if (area > kEpsilon) {
        under.emplace_back(j, area);
        supported += area;
      }
    }
    if (supported / (p.length * p.width) < options.min_support - 1e-6) {
      return describe(p) + " is not sufficiently supported";
    }
    for (const auto& [j, area] : under) {
      carried[j] += (b.weight + carried[i]) * area / supported;
    }
  }
  return {};
}

}  // namespace cell::algo
