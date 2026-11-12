#include "sensor_sim/frame_generator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <stdexcept>

namespace cell::sensor {
namespace {

// The wire format is little-endian float32 and rows are memcpy'd as-is.
static_assert(std::endian::native == std::endian::little, "sensor_sim assumes little-endian");
static_assert(sizeof(float) == 4, "sensor_sim assumes IEEE-754 binary32 float");

// splitmix64 finaliser: decorrelates consecutive frame indices into noise seeds.
std::uint32_t seed_for_frame(std::uint64_t frame_index) noexcept {
  std::uint64_t z = frame_index + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return static_cast<std::uint32_t>((z ^ (z >> 31)) >> 32);
}

}  // namespace

FrameGenerator::FrameGenerator(std::uint32_t width, std::uint32_t height, double fps,
                               SceneConfig scene)
    : width_(width), height_(height), fps_(fps), scene_(scene) {
  if (width == 0 || height == 0) {
    throw std::invalid_argument("frame width and height must be non-zero");
  }
  if (!(fps > 0.0)) {
    throw std::invalid_argument("fps must be positive");
  }
  if (!(scene_.camera_height_m > scene_.box_height_m) || scene_.box_height_m < 0.0 ||
      !(scene_.horizontal_fov_deg > 0.0 && scene_.horizontal_fov_deg < 180.0)) {
    throw std::invalid_argument("invalid scene geometry");
  }
  const double half_fov = scene_.horizontal_fov_deg * std::numbers::pi / 360.0;
  focal_px_ = 0.5 * width_ / std::tan(half_fov);
  half_view_x_m_ = scene_.camera_height_m * std::tan(half_fov);

  slope_x_.resize(width_);
  slope_y_.resize(height_);
  const double cx = 0.5 * (width_ - 1);
  const double cy = 0.5 * (height_ - 1);
  for (std::uint32_t u = 0; u < width_; ++u) {
    slope_x_[u] = static_cast<float>((u - cx) / focal_px_);
  }
  for (std::uint32_t v = 0; v < height_; ++v) {
    slope_y_[v] = static_cast<float>((v - cy) / focal_px_);
  }
  row_.resize(static_cast<std::size_t>(width_) * 3);
}

double FrameGenerator::box_center_x(std::uint64_t frame_index) const noexcept {
  // The box enters just outside the left edge of the view, leaves just past
  // the right edge, then wraps.
  const double start = -half_view_x_m_ - 0.5 * scene_.box_length_m;
  const double travel = 2.0 * half_view_x_m_ + scene_.box_length_m;
  const double t = static_cast<double>(frame_index) / fps_;
  return start + std::fmod(t * scene_.belt_speed_mps, travel);
}

void FrameGenerator::generate(std::uint64_t frame_index, std::span<std::byte> out) {
  if (out.size() != frame_bytes()) {
    throw std::invalid_argument("output buffer size does not match the frame size");
  }
  const auto belt_z = static_cast<float>(scene_.camera_height_m);
  const auto box_z = static_cast<float>(scene_.camera_height_m - scene_.box_height_m);
  const double box_cx = box_center_x(frame_index);
  const double box_x0 = box_cx - 0.5 * scene_.box_length_m;
  const double box_x1 = box_cx + 0.5 * scene_.box_length_m;
  const double half_box_y = 0.5 * scene_.box_width_m;

  // Columns whose ray meets the box's top face, [u0, u1). Only the top face
  // is modelled; the sides are a sub-pixel sliver from this viewpoint.
  const double cx = 0.5 * (width_ - 1);
  const auto column_at = [&](double x) {
    const double u = std::ceil(x / box_z * focal_px_ + cx);
    return static_cast<std::uint32_t>(std::clamp(u, 0.0, static_cast<double>(width_)));
  };
  const std::uint32_t u0 = column_at(box_x0);
  const std::uint32_t u1 = column_at(box_x1);

  // Uniform noise in [-sigma, sigma) from a 32-bit LCG: one multiply-add per
  // point, and reproducible because the seed depends only on the frame index.
  std::uint32_t rng = seed_for_frame(frame_index);
  const float noise_scale = static_cast<float>(scene_.noise_sigma_m / 2147483648.0);

  const std::size_t row_bytes = row_.size() * sizeof(float);
  float* const row = row_.data();
  for (std::uint32_t v = 0; v < height_; ++v) {
    const float sy = slope_y_[v];
    const bool box_row = std::abs(static_cast<double>(sy) * box_z) <= half_box_y;
    const std::uint32_t b0 = box_row ? u0 : 0;
    const std::uint32_t b1 = box_row ? u1 : 0;
    for (std::uint32_t u = 0; u < width_; ++u) {
      rng = rng * 1664525U + 1013904223U;
      const float noise = static_cast<float>(static_cast<std::int32_t>(rng)) * noise_scale;
      const float z = ((u >= b0 && u < b1) ? box_z : belt_z) + noise;
      row[3 * u] = slope_x_[u] * z;
      row[3 * u + 1] = sy * z;
      row[3 * u + 2] = z;
    }
    std::memcpy(out.data() + static_cast<std::size_t>(v) * row_bytes, row, row_bytes);
  }
}

}  // namespace cell::sensor
