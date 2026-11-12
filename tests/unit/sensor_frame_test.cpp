#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "sensor_sim/disconnect_fault.hpp"
#include "sensor_sim/frame_generator.hpp"

namespace {

using cell::sensor::DisconnectFault;
using cell::sensor::FrameGenerator;
using cell::sensor::SceneConfig;

constexpr std::uint64_t kMs = 1'000'000ULL;

struct Point {
  float x, y, z;
};

Point point_at(const std::vector<std::byte>& buf, std::size_t index) {
  Point p{};
  std::memcpy(&p, buf.data() + index * cell::sensor::kBytesPerPoint, sizeof(p));
  return p;
}

TEST(SensorFrame, FrameSizeIsTwelveBytesPerPoint) {
  EXPECT_EQ(cell::sensor::kBytesPerPoint, 12U);
  EXPECT_EQ(FrameGenerator(640, 480, 15.0).frame_bytes(), 640U * 480U * 12U);  // 3,686,400
  EXPECT_EQ(FrameGenerator(1, 1, 15.0).frame_bytes(), 12U);
  EXPECT_EQ(FrameGenerator(7, 3, 15.0).frame_bytes(), 7U * 3U * 12U);
}

TEST(SensorFrame, RejectsBadArguments) {
  EXPECT_THROW(FrameGenerator(0, 480, 15.0), std::invalid_argument);
  EXPECT_THROW(FrameGenerator(640, 0, 15.0), std::invalid_argument);
  EXPECT_THROW(FrameGenerator(640, 480, 0.0), std::invalid_argument);

  FrameGenerator gen(64, 48, 15.0);
  std::vector<std::byte> small(gen.frame_bytes() - 1);
  EXPECT_THROW(gen.generate(0, small), std::invalid_argument);
  std::vector<std::byte> big(gen.frame_bytes() + 12);
  EXPECT_THROW(gen.generate(0, big), std::invalid_argument);
}

TEST(SensorFrame, SameIndexGivesIdenticalBytes) {
  FrameGenerator a(160, 120, 15.0);
  FrameGenerator b(160, 120, 15.0);
  std::vector<std::byte> f1(a.frame_bytes());
  std::vector<std::byte> f2(a.frame_bytes());
  std::vector<std::byte> f3(a.frame_bytes());

  a.generate(42, f1);
  a.generate(7, f3);  // generating other frames in between must not matter
  a.generate(42, f2);
  EXPECT_EQ(f1, f2);
  b.generate(42, f3);  // nor which generator instance produced it
  EXPECT_EQ(f1, f3);
}

TEST(SensorFrame, FramesVaryOverTime) {
  FrameGenerator gen(160, 120, 15.0);
  std::vector<std::byte> f0(gen.frame_bytes());
  std::vector<std::byte> f1(gen.frame_bytes());
  gen.generate(0, f0);
  gen.generate(1, f1);
  EXPECT_NE(f0, f1);
}

TEST(SensorFrame, ValuesAreFiniteAndPlausible) {
  const SceneConfig scene;
  FrameGenerator gen(320, 240, 15.0, scene);
  std::vector<std::byte> buf(gen.frame_bytes());
  const float belt = static_cast<float>(scene.camera_height_m);
  const float box_top = static_cast<float>(scene.camera_height_m - scene.box_height_m);
  const float tol = static_cast<float>(scene.noise_sigma_m) + 1e-5F;

  for (const std::uint64_t index : {0ULL, 1ULL, 37ULL, 1000ULL, 123456789ULL}) {
    gen.generate(index, buf);
    std::size_t box_points = 0;
    for (std::size_t i = 0; i < 320U * 240U; ++i) {
      const Point p = point_at(buf, i);
      ASSERT_TRUE(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
      const bool on_belt = std::abs(p.z - belt) <= tol;
      const bool on_box = std::abs(p.z - box_top) <= tol;
      ASSERT_TRUE(on_belt || on_box) << "frame " << index << " point " << i << " z=" << p.z;
      box_points += on_box ? 1 : 0;
      // 60 degree horizontal FOV at <= 1.5 m: |x| stays under ~0.87 m.
      ASSERT_LT(std::abs(p.x), 1.0F);
      ASSERT_LT(std::abs(p.y), 1.0F);
    }
    // The box never covers more than a small part of the view.
    EXPECT_LT(box_points, 320U * 240U / 4);
  }
}

TEST(SensorFrame, CloudIsOrganisedRowMajor) {
  FrameGenerator gen(64, 48, 15.0);
  std::vector<std::byte> buf(gen.frame_bytes());
  gen.generate(3, buf);
  // x grows along a row, y grows down a column; the optical centre is ~0.
  EXPECT_LT(point_at(buf, 0).x, point_at(buf, 63).x);
  EXPECT_LT(point_at(buf, 0).y, point_at(buf, 47U * 64U).y);
  EXPECT_LT(point_at(buf, 0).x, 0.0F);
  EXPECT_GT(point_at(buf, 63).x, 0.0F);
  EXPECT_NEAR(point_at(buf, 0).y, point_at(buf, 63).y, 0.01F);  // same row
}

TEST(SensorFrame, BoxMovesAlongTheBeltAndWraps) {
  const SceneConfig scene;
  FrameGenerator gen(320, 240, 15.0, scene);
  // 0.5 m/s at 15 fps: 1/30 m per frame.
  EXPECT_NEAR(gen.box_center_x(1) - gen.box_center_x(0), 0.5 / 15.0, 1e-9);
  EXPECT_NEAR(gen.box_center_x(30) - gen.box_center_x(0), 1.0, 1e-9);
  // It wraps: over a long run the position stays bounded.
  double lo = 1e9;
  double hi = -1e9;
  for (std::uint64_t i = 0; i < 2000; ++i) {
    lo = std::min(lo, gen.box_center_x(i));
    hi = std::max(hi, gen.box_center_x(i));
  }
  EXPECT_GT(lo, -1.5);
  EXPECT_LT(hi, 1.5);
  EXPECT_LT(lo, -0.5);  // and it really sweeps the view
  EXPECT_GT(hi, 0.5);

  // When the box is centred under the camera, the centre pixel sees its top.
  std::uint64_t centred = 0;
  for (std::uint64_t i = 0; i < 2000; ++i) {
    if (std::abs(gen.box_center_x(i)) < std::abs(gen.box_center_x(centred))) {
      centred = i;
    }
  }
  std::vector<std::byte> buf(gen.frame_bytes());
  gen.generate(centred, buf);
  const Point centre = point_at(buf, 120U * 320U + 160U);
  EXPECT_NEAR(centre.z, scene.camera_height_m - scene.box_height_m, 0.002);
  const Point corner = point_at(buf, 0);
  EXPECT_NEAR(corner.z, scene.camera_height_m, 0.002);
}

TEST(SensorDisconnect, InactiveByDefault) {
  DisconnectFault fault;
  EXPECT_FALSE(fault.active(0));
  EXPECT_FALSE(fault.active(123 * kMs));
  fault.reconnect();  // harmless when nothing is active
  EXPECT_FALSE(fault.active(0));
}

TEST(SensorDisconnect, TimedFaultExpiresOnItsOwn) {
  DisconnectFault fault;
  fault.disconnect(1000 * kMs, 500);
  EXPECT_TRUE(fault.active(1000 * kMs));
  EXPECT_TRUE(fault.active(1499 * kMs));
  EXPECT_FALSE(fault.active(1500 * kMs));
  EXPECT_FALSE(fault.active(1400 * kMs));  // once expired it stays cleared
  EXPECT_FALSE(fault.active(9999 * kMs));
}

TEST(SensorDisconnect, ZeroDurationLastsUntilReconnect) {
  DisconnectFault fault;
  fault.disconnect(1000 * kMs, 0);
  EXPECT_TRUE(fault.active(1000 * kMs));
  EXPECT_TRUE(fault.active(1000 * kMs + 3'600'000 * kMs));  // an hour later
  fault.reconnect();
  EXPECT_FALSE(fault.active(1000 * kMs + 3'600'001 * kMs));
}

TEST(SensorDisconnect, ReconnectEndsTimedFaultEarly) {
  DisconnectFault fault;
  fault.disconnect(0, 10'000);
  EXPECT_TRUE(fault.active(5000 * kMs));
  fault.reconnect();
  EXPECT_FALSE(fault.active(5001 * kMs));
}

TEST(SensorDisconnect, NewDisconnectRestartsTheTimer) {
  DisconnectFault fault;
  fault.disconnect(0, 100);
  fault.disconnect(80 * kMs, 100);
  EXPECT_TRUE(fault.active(150 * kMs));
  EXPECT_FALSE(fault.active(180 * kMs));

  // A timed fault can be replaced by an indefinite one and vice versa.
  fault.disconnect(200 * kMs, 0);
  EXPECT_TRUE(fault.active(100'000 * kMs));
  fault.disconnect(100'000 * kMs, 50);
  EXPECT_FALSE(fault.active(100'050 * kMs));
}

TEST(SensorDisconnect, MaxDurationDoesNotOverflow) {
  DisconnectFault fault;
  fault.disconnect(5 * kMs, 0xFFFFFFFFU);
  EXPECT_TRUE(fault.active(5 * kMs + 0xFFFFFFFEULL * kMs));
  EXPECT_FALSE(fault.active(5 * kMs + 0xFFFFFFFFULL * kMs));
}

}  // namespace
