#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace cell::sensor {

// Bytes per point on the wire: three little-endian float32 (x, y, z).
inline constexpr std::size_t kBytesPerPoint = 3 * sizeof(float);

// Geometry of the synthetic scene, in metres. A depth camera looks straight
// down at a conveyor belt; a box rides the belt along +x and wraps around.
struct SceneConfig {
  double camera_height_m = 1.5;  // camera above the belt surface
  double horizontal_fov_deg = 60.0;
  double belt_speed_mps = 0.5;
  double box_length_m = 0.30;  // along x
  double box_width_m = 0.20;   // along y
  double box_height_m = 0.15;
  double noise_sigma_m = 0.001;  // amplitude of the uniform depth noise
};

// Generates organised point clouds (row-major, width*height points) of the
// scene. A frame is a pure function of its index: generate(i) always produces
// the same bytes, so frames are reproducible in tests and across runs.
//
// The hot path allocates nothing: per-column/row ray slopes and one scratch
// row are sized in the constructor, and the cloud is written straight into
// the caller's buffer.
class FrameGenerator {
 public:
  FrameGenerator(std::uint32_t width, std::uint32_t height, double fps, SceneConfig scene = {});

  std::uint32_t width() const noexcept { return width_; }
  std::uint32_t height() const noexcept { return height_; }
  std::size_t frame_bytes() const noexcept {
    return static_cast<std::size_t>(width_) * height_ * kBytesPerPoint;
  }

  // Centre of the box along x at the given frame (exposed for tests).
  double box_center_x(std::uint64_t frame_index) const noexcept;

  // Fill `out` (exactly frame_bytes() long, else std::invalid_argument) with
  // frame `frame_index`.
  void generate(std::uint64_t frame_index, std::span<std::byte> out);

 private:
  std::uint32_t width_;
  std::uint32_t height_;
  double fps_;
  SceneConfig scene_;
  double focal_px_;
  double half_view_x_m_;        // half the belt length visible at belt depth
  std::vector<float> slope_x_;  // x/z of the ray through each column
  std::vector<float> slope_y_;  // y/z of the ray through each row
  std::vector<float> row_;      // scratch: one row of xyz triples
};

}  // namespace cell::sensor
