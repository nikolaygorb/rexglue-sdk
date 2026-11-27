/**
 * @file        graphics/scene_contract_test.cpp
 * @brief       Title scene contract (protocol 4) codec and scene history math
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string>

#include <rex/graphics/native/scene_history.h>
#include <rex/graphics/title_scene.h>

namespace title = rex::graphics::title;
namespace scene = rex::graphics::native::scene;

namespace {
// Direct3D left-handed perspective, z/w 0 at near (or at far when reversed).
scene::Matrix Perspective(float fov_y, float aspect, float near_z, float far_z, bool reversed) {
  const float y = 1.f / std::tan(fov_y / 2), x = y / aspect;
  if (reversed) std::swap(near_z, far_z);
  const float a = far_z / (far_z - near_z), b = -near_z * far_z / (far_z - near_z);
  return {x, 0, 0, 0, 0, y, 0, 0, 0, 0, a, b, 0, 0, 1, 0};
}
scene::View MakeView(const scene::Matrix& projection) {
  scene::View v;
  v.session = 1; v.frame = 10; v.width = 1280; v.height = 720; v.scale4 = 4;
  v.projection = projection; v.pv = projection;
  v.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  v.ndc_scale = {1, -1, 1}; v.ndc_offset = {0, 0, 0}; v.depth_range = {0, 1};
  return v;
}
std::array<double, 2> Motion(const scene::ReprojectionConstants& c, double u, double v, double depth, double w,
                             double h) {
  double p[4]{};
  for (size_t y = 0; y < 4; ++y)
    p[y] = c.current_to_previous[y] * u + c.current_to_previous[4 + y] * v + c.current_to_previous[8 + y] * depth +
           c.current_to_previous[12 + y];
  return {(p[0] / p[3] - (u - c.jitter[0])) * w, (p[1] / p[3] - (v - c.jitter[1])) * h};
}
}  // namespace

TEST_CASE("Scene protocol 4 round trip and validation", "[graphics][scene]") {
  title::SceneMarker marker, decoded;
  marker.point = title::ScenePoint::kView; marker.frame = 5; marker.session = 7;
  marker.flags = title::kSceneCamera | title::kSceneReset; marker.width = 1216; marker.height = 640;
  marker.delta_time = 1.f / 60; marker.camera_register = 7; marker.locked_distance = 50;
  marker.camera.projection[0] = 2;
  auto words = title::EncodeScene(marker);
  REQUIRE(title::DecodeScene(words, decoded));
  CHECK(decoded.camera_register == 7);
  CHECK(decoded.delta_time == marker.delta_time);
  CHECK(decoded.camera.projection[0] == 2);
  auto bad = words; bad[0] = 2;
  CHECK_FALSE(title::DecodeScene(bad, decoded));
  bad = words; bad[1] = uint32_t(title::ScenePoint::kFrameEnd);
  CHECK_FALSE(title::DecodeScene(bad, decoded));  // Camera only on kView.
  bad = words; bad[40] = 0x7F800000;
  CHECK_FALSE(title::DecodeScene(bad, decoded));
  bad = words; bad[19] = 1280;  // Width without height.
  CHECK_FALSE(title::DecodeScene(bad, decoded));
  // Version 3: words 18-19 and kSceneNoFrameGeneration reserved.
  auto v3 = words; v3[0] = 3;
  REQUIRE(title::DecodeScene(v3, decoded));
  v3[18] = 1;
  CHECK_FALSE(title::DecodeScene(v3, decoded));
  v3 = words; v3[0] = 3; v3[6] |= title::kSceneNoFrameGeneration;
  CHECK_FALSE(title::DecodeScene(v3, decoded));
}

TEST_CASE("Scene protocol 4 frame rectangle and no-generation flag", "[graphics][scene]") {
  title::SceneMarker marker, decoded;
  marker.point = title::ScenePoint::kPostEnd; marker.frame = 9; marker.session = 3;
  marker.flags = title::kSceneNoFrameGeneration; marker.width = 1280; marker.height = 720;
  marker.frame_x = 32; marker.frame_y = 40; marker.frame_width = 1216; marker.frame_height = 640;
  REQUIRE(title::DecodeScene(title::EncodeScene(marker), decoded));
  CHECK(decoded.flags == title::kSceneNoFrameGeneration);
  CHECK(decoded.frame_x == 32);
  CHECK(decoded.frame_y == 40);
  CHECK(decoded.frame_width == 1216);
  CHECK(decoded.frame_height == 640);
}

TEST_CASE("Scene protocol 4 locked volume", "[graphics][scene]") {
  title::SceneMarker marker, decoded;
  marker.point = title::ScenePoint::kLockedVolume; marker.frame = 4; marker.session = 2;
  marker.width = 1216; marker.height = 640;
  marker.locked_box = {-50.f, -180.f, -400.f, 120.f, 30.f, -250.f};
  auto words = title::EncodeScene(marker);
  REQUIRE(title::DecodeScene(words, decoded));
  CHECK(decoded.point == title::ScenePoint::kLockedVolume);
  CHECK(decoded.locked_box == marker.locked_box);
  auto bad = words; bad[20] = std::bit_cast<uint32_t>(500.f);  // min x > max x.
  CHECK_FALSE(title::DecodeScene(bad, decoded));
  bad = words; bad[0] = 3;  // Not in protocol 3.
  CHECK_FALSE(title::DecodeScene(bad, decoded));
}

TEST_CASE("Pixel to view space inverts the view's mapping", "[graphics][scene]") {
  scene::View v;
  v.width = 64; v.height = 32;
  v.ndc_scale = {1, -1, 1}; v.ndc_offset = {0, 0, 0}; v.depth_range = {0, 1};
  // Right-handed perspective, w = -z, near 10, far 1000 (Direct3D z/w in [0, 1]).
  const double n = 10, f = 1000;
  v.projection = {1.f, 0, 0, 0,  0, 2.f, 0, 0,  0, 0, float(f / (n - f)), float(n * f / (n - f)),  0, 0, -1.f, 0};
  std::array<float, 16> m{};
  REQUIRE(scene::PixelToView(v, m));
  // The view-space point (10, -5, -100) seen through the projection and mapping.
  const double px = 10, py = -5, pz = -100;
  const double cw = -pz, cx = px * 1 / cw, cy = py * 2 / cw;
  const double cz = (f / (n - f) * pz + n * f / (n - f)) / cw;
  const double u = (cx * 1 + 1) / 2, vv = (cy * -1 + 1) / 2;
  double out[4]{};
  for (size_t y = 0; y < 4; ++y) out[y] = m[y] * u + m[4 + y] * vv + m[8 + y] * cz + m[12 + y];
  CHECK(out[0] / out[3] == Catch::Approx(px).epsilon(1e-3));
  CHECK(out[1] / out[3] == Catch::Approx(py).epsilon(1e-3));
  CHECK(out[2] / out[3] == Catch::Approx(pz).epsilon(1e-3));
}

TEST_CASE("Camera facts from the projection and host mapping", "[graphics][scene]") {
  for (bool reversed : {false, true}) {
    auto view = MakeView(Perspective(1.f, 16.f / 9, 5, 1000, reversed));
    const auto camera = scene::DeriveCamera(view);
    REQUIRE(camera.valid);
    CHECK(std::abs(camera.near_distance - 5) < 1e-3);
    CHECK(std::abs(camera.far_distance - 1000) < 1);
    CHECK(std::abs(camera.fov_y - 1) < 1e-5);
    CHECK(camera.depth_inverted == reversed);
    CHECK(camera.far_host_depth == (reversed ? 0.f : 1.f));
    view.locked_distance = 50;
    const float locked = scene::LockedHostDepth(view, camera);
    CHECK((reversed ? locked > .0f && locked < .2f : locked > .8f && locked < 1.f));
  }
}

TEST_CASE("Jitter is excluded from camera motion", "[graphics][scene]") {
  auto previous = MakeView(Perspective(1.f, 16.f / 9, 5, 1000, false));
  auto current = previous; current.frame = 11; current.jitter = scene::Jitter(3, 8);
  scene::ReprojectionConstants constants;
  REQUIRE(scene::MakeReprojection(current, previous, true, constants));
  const auto m = Motion(constants, .3, .6, .7, 1280, 720);
  CHECK(std::abs(m[0]) < 1e-3);
  CHECK(std::abs(m[1]) < 1e-3);
  REQUIRE(scene::MakeReprojection(current, previous, false, constants));
  CHECK(constants.current_to_previous == std::array<float, 16>{});
  current.title_reset = true;
  CHECK(std::string(scene::ResetReason(current, &previous)) == "title");
}

// Recorded right-handed projection (w = -z) of a shipped title; host depth .5 (near) -> 0 (far).
constexpr scene::Matrix kRecordedProjection{1.353036f, 0, 0, 0, 0, 2.405397f, 0, 0,
                                            0, 0, -1.00000727f, -3.64285278f, 0, 0, -1, 0};

TEST_CASE("History resets and camera motion", "[graphics][scene]") {
  scene::Matrix inverse;
  REQUIRE(scene::Invert(kRecordedProjection, inverse));
  const auto identity = scene::Multiply(kRecordedProjection, inverse);
  for (size_t i = 0; i < 16; ++i) CHECK(std::abs(identity[i] - scene::Identity()[i]) < 1e-5f);
  CHECK_FALSE(scene::Invert(scene::Matrix{}, inverse));
  auto nonfinite = kRecordedProjection; nonfinite[0] = std::numeric_limits<float>::infinity();
  CHECK_FALSE(scene::Invert(nonfinite, inverse));

  scene::View old; old.session = 1; old.frame = 8; old.width = 2432; old.height = 1280; old.scale4 = 8;
  old.color_id = 3; old.depth_id = 4; old.pv = scene::Identity();
  std::copy_n(old.pv.begin(), 12, old.view.begin()); old.ndc_scale = {1, -1, 1}; old.depth_range = {.5f, 0};
  auto now = old; ++now.frame;
  CHECK_FALSE(scene::ResetReason(now, &old));
  CHECK(scene::ResetReason(now, nullptr));
  auto changed = now; changed.frame += 1; CHECK(scene::ResetReason(changed, &old));  // Gap.
  changed = now; changed.session += 1; CHECK(scene::ResetReason(changed, &old));
  changed = now; changed.width /= 2; CHECK(scene::ResetReason(changed, &old));
  changed = now; changed.depth_id += 1; CHECK(scene::ResetReason(changed, &old));
  changed = now; changed.depth_range = {0, 1}; CHECK(scene::ResetReason(changed, &old));
  changed = now; changed.view[0] = -1; CHECK(scene::ResetReason(changed, &old));  // Orientation flip.

  // Clip x translation of a quarter -> -304 px at 2432, no vertical motion.
  scene::ReprojectionConstants constants;
  now.pv[3] = .25f;
  REQUIRE(scene::MakeReprojection(now, old, true, constants));
  const auto m = Motion(constants, .5, .5, .25, 2432, 1280);
  CHECK(std::abs(m[0] + 304) < 1e-3);
  CHECK(std::abs(m[1]) < 1e-3);
  CHECK(constants.depth[0] == 0);
  CHECK(constants.depth[1] == .5f);
  // Provider depth: host .5 (near) -> clip z/w 0, host 0 (far) -> 1.
  CHECK(constants.ndc_depth[0] == -2.f);
  CHECK(constants.ndc_depth[1] == 1.f);
  changed = now; changed.ndc_offset[1] = std::numeric_limits<float>::quiet_NaN();
  CHECK_FALSE(scene::MakeReprojection(changed, old, true, constants));
  changed = now; changed.depth_range = {0, 0};
  CHECK_FALSE(scene::MakeReprojection(changed, old, true, constants));
}

TEST_CASE("Camera far from the origin keeps motion precise", "[graphics][scene]") {
  // Recorded P x V of a stationary and a turning camera ~10 km from the origin.
  // A float relative PV lost 2^-12 in Z/W and gave false motion everywhere.
  scene::View old; old.session = 1; old.frame = 8; old.width = 2432; old.height = 1280; old.scale4 = 8;
  old.depth_id = 4; old.ndc_scale = {1, -1, 1}; old.depth_range = {.5f, 0};
  old.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  old.pv = {1.04360509f, 0, .861158848f, 12541.8018f, .0522937216f, 2.40399361f, -.0633727461f, 1208.53149f,
            .636097372f, -.0341579542f, -.770861685f, 8093.73242f, .636092782f, -.0341577083f, -.770856082f,
            8097.31641f};
  auto now = old; ++now.frame;
  scene::ReprojectionConstants constants;
  REQUIRE(scene::MakeReprojection(now, old, true, constants));
  auto m = Motion(constants, .41, .63, .00001, 2432, 1280);
  CHECK(std::abs(m[0]) < 1e-3);
  CHECK(std::abs(m[1]) < 1e-3);

  old.pv = {1.09755647f, 0, .791249633f, 13230.9492f, .0458891951f, 2.40411687f, -.0636537299f, 1129.81604f,
            .58448869f, -.0326228961f, -.810754538f, 7472.94629f, .584484458f, -.0326226614f, -.810748696f,
            7476.53516f};
  now.pv = {1.10119092f, 0, .786183715f, 13277.501f, .0455954149f, 2.40411687f, -.0638645515f, 1126.27429f,
            .580746472f, -.0326229148f, -.81343931f, 7427.83008f, .5807423f, -.0326226801f, -.813433409f,
            7431.41895f};
  // Independent numpy.linalg.solve(current.T, previous.T).T in float64 (row-major).
  const double relative[16]{.9999893308,   -.0000845579561,   .001800019876,  -.008031501435,
                            .0002672479604, 1.,               .00002545373718, -.00002633274926,
                            .003403943498, -.0000001363569453, 1.000158548,    -.0001691513171,
                            .003403882030, -.0000001361686515, -.00003712079706, 1.000026464};
  REQUIRE(scene::MakeReprojection(now, old, true, constants));
  for (double u : {.1, .5, .9}) {
    for (double depth : {.00001, .0005}) {
      // uv/depth -> guest NDC (scale {1,-1,1}, range .5 -> 0) -> relative -> previous uv.
      const double ndc[4]{u * 2 - 1, -(.4 * 2 - 1), (depth - .5) / -.5, 1};
      double p[4]{};
      for (size_t y = 0; y < 4; ++y)
        for (size_t x = 0; x < 4; ++x) p[y] += relative[y * 4 + x] * ndc[x];
      const double expected_x = ((p[0] / p[3]) * .5 + .5 - u) * 2432;
      const double expected_y = ((-(p[1] / p[3])) * .5 + .5 - .4) * 1280;
      m = Motion(constants, u, .4, depth, 2432, 1280);
      CHECK(std::abs(m[0] - expected_x) < 2e-3);
      CHECK(std::abs(m[1] - expected_y) < 2e-3);
    }
  }
}

TEST_CASE("Recorded right-handed projection, jitter phases, slot retirement", "[graphics][scene]") {
  auto view = MakeView(kRecordedProjection);
  view.depth_range = {.5f, 0};
  const auto camera = scene::DeriveCamera(view);
  REQUIRE(camera.valid);
  CHECK(std::abs(camera.near_distance - 3.6428) < 1e-3);
  CHECK(camera.far_distance > 4e5);
  CHECK(camera.depth_inverted);
  CHECK(camera.far_host_depth == 0);
  CHECK(camera.near_host_depth == .5f);
  CHECK(std::abs(camera.fov_y - 2 * std::atan(1 / 2.405397)) < 1e-6);
  view.locked_distance = 60;
  const float locked = scene::LockedHostDepth(view, camera);
  CHECK((locked > .02f && locked < .04f));

  // Halton (2, 3), wrapping at the phase count.
  CHECK(scene::Jitter(0, 0) == std::array<float, 2>{0, 0});
  const auto first = scene::Jitter(0, 8);
  CHECK(first[0] == 0);
  CHECK(std::abs(first[1] + 1.f / 6) < 1e-6f);
  CHECK(scene::Jitter(8, 8) == first);

  // A slot is reused only once its last reader completed; the previous frame stays.
  const std::array<uint64_t, 4> readers{7, 11, 9, 15};
  CHECK(scene::FreeSlot(readers, 0, 8) == size_t(4));
  CHECK(scene::FreeSlot(readers, 0, 9) == size_t(2));
  CHECK(scene::FreeSlot(readers, 2, 7) == size_t(0));
}

TEST_CASE("View and projection must match view_projection", "[graphics][scene]") {
  const auto projection = Perspective(1.f, 1.f, 1, 100, false);
  const std::array<float, 12> view{1, 0, 0, 3, 0, 1, 0, -2, 0, 0, 1, 10};
  const auto pv = scene::Multiply(projection, {1, 0, 0, 3, 0, 1, 0, -2, 0, 0, 1, 10, 0, 0, 0, 1});
  CHECK(scene::CameraConsistent(view, projection, pv));
  auto wrong = pv; wrong[3] += 1;
  CHECK_FALSE(scene::CameraConsistent(view, projection, wrong));
}
