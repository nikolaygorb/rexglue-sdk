// Ordered title scene contract (PM4 type-3 opcode 0x66, owned payload, no host pointers).
// A title producer annotates its own command stream; the GPU backend verifies the
// annotation against the work it actually executes. Title knowledge (where the camera
// lives, which shader draws the world) stays in the title; this header is generic.
#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rex::graphics::title {
inline constexpr uint32_t kSceneOpcode = 0x66;
// 4: frame rectangle (words 18-19), kSceneNoFrameGeneration. Version 3 markers
// are still accepted (those words zero). A producer checks the backend's
// version is at least the one it was built with.
inline constexpr uint32_t kSceneProtocolVersion = 4;
inline constexpr uint32_t kSceneProtocolVersionMin = 3;
inline constexpr size_t kSceneWords = 64;
inline constexpr uint32_t kNoRegister = 0xFFFFFFFFu;

// Marker flags.
inline constexpr uint32_t kSceneCamera = 1;       // kView: camera block is valid.
inline constexpr uint32_t kSceneCapture = 2;      // Persist diagnostics for this frame (requested captures).
inline constexpr uint32_t kSceneReset = 4;        // kView: history discontinuity (cut, teleport, load).
inline constexpr uint32_t kSceneAutoOpaque = 8;   // kView: snapshot opaque color before the first blended scene draw.
// Any point of a frame: no generated frame before this one (frame generation):
// cuts the title can't flag otherwise, menus, frame rates too low for it.
inline constexpr uint32_t kSceneNoFrameGeneration = 16;
inline constexpr uint32_t kSceneKnownFlags = 31;

// Points in the title's frame, in stream order. kView arrives after the first
// draw that uses the camera; kSceneEnd once scene color/depth are final
// (post-processing and HUD follow); kPostEnd before the HUD; kOpaqueEnd is an
// explicit opaque/translucent boundary for titles that know it. kProbe is
// diagnostic only (with kSceneCapture: target snapshot, no frame action).
// kLockedVolume (protocol 4), after kView and before kSceneEnd: a view-space
// box whose pixels move with the camera (a third-person character the camera
// follows), up to 4 a frame; their motion is zero like the camera-locked
// distance's.
enum class ScenePoint : uint32_t {
  kView = 1, kSceneEnd = 2, kPostEnd = 3, kFrameEnd = 4, kOpaqueEnd = 5, kProbe = 6, kLockedVolume = 7
};

// Matrices are row-major with column vectors (clip = projection * view * world),
// guest clip space (Direct3D: z/w in [0, 1]).
struct SceneCamera {
  std::array<float, 12> view{};             // 3x4 world -> view.
  std::array<float, 16> projection{};       // 4x4 view -> clip.
  std::array<float, 16> view_projection{};  // projection * view, as the shaders use it.
};

struct SceneMarker {
  ScenePoint point{};
  uint64_t frame = 0, session = 0;  // Title render frame, consecutive; session per run.
  uint32_t flags = 0, view_id = 0;  // view_id 0 = main view.
  uint32_t width = 0, height = 0;   // Scene viewport at the target origin, guest pixels.
  float delta_time = 0;             // Simulated seconds since the previous frame; 0 = unknown.
  uint32_t reset_reason = 0;        // Title-defined, nonzero with kSceneReset; diagnostic.
  // VS float constant register holding view_projection rows (4 registers); the
  // backend verifies the active shader reads them and they equal the payload.
  uint32_t camera_register = kNoRegister;
  // View-space distance below which pixels are camera-locked (first-person
  // weapon): their motion is zero. 0 = off.
  float locked_distance = 0;
  float meters_per_unit = 0;  // World/view units to meters (FSR scale hints); 0 = unknown.
  // Where the scene viewport appears in the guest's front buffer, guest
  // pixels (the title's composition may move or stretch it). Any point; all 0:
  // unknown, taken as at the origin 1:1. Protocol 4.
  uint16_t frame_x = 0, frame_y = 0, frame_width = 0, frame_height = 0;
  // kLockedVolume: view space (the camera block's view) min xyz, max xyz.
  std::array<float, 6> locked_box{};
  uint32_t device = 0;       // Opaque guest identity, diagnostic only.
  uint64_t source_hash = 0;  // Opaque title shader identity, diagnostic only.
  SceneCamera camera{};
};

inline std::array<uint32_t, kSceneWords> EncodeScene(const SceneMarker& m) {
  std::array<uint32_t, kSceneWords> w{};
  w[0] = kSceneProtocolVersion; w[1] = uint32_t(m.point);
  w[2] = uint32_t(m.frame); w[3] = uint32_t(m.frame >> 32);
  w[4] = uint32_t(m.session); w[5] = uint32_t(m.session >> 32);
  w[6] = m.flags; w[7] = m.view_id; w[8] = m.width; w[9] = m.height;
  w[10] = std::bit_cast<uint32_t>(m.delta_time); w[11] = m.reset_reason;
  w[12] = m.camera_register; w[13] = std::bit_cast<uint32_t>(m.locked_distance);
  w[14] = m.device; w[15] = std::bit_cast<uint32_t>(m.meters_per_unit); w[16] = uint32_t(m.source_hash); w[17] = uint32_t(m.source_hash >> 32);
  w[18] = uint32_t(m.frame_x) | uint32_t(m.frame_y) << 16;
  w[19] = uint32_t(m.frame_width) | uint32_t(m.frame_height) << 16;
  if (m.point == ScenePoint::kLockedVolume) {
    for (size_t i = 0; i < 6; ++i) w[20 + i] = std::bit_cast<uint32_t>(m.locked_box[i]);
    return w;
  }
  for (size_t i = 0; i < 12; ++i) w[20 + i] = std::bit_cast<uint32_t>(m.camera.view[i]);
  for (size_t i = 0; i < 16; ++i) w[32 + i] = std::bit_cast<uint32_t>(m.camera.projection[i]);
  for (size_t i = 0; i < 16; ++i) w[48 + i] = std::bit_cast<uint32_t>(m.camera.view_projection[i]);
  return w;
}

inline bool DecodeScene(std::span<const uint32_t> w, SceneMarker& m) {
  if (w.size() != kSceneWords || w[0] < kSceneProtocolVersionMin || w[0] > kSceneProtocolVersion ||
      w[1] < 1 || w[1] > 7 || (w[0] < 4 && w[1] == 7)) return false;
  if ((!w[2] && !w[3]) || (!w[4] && !w[5])) return false;
  if (!w[8] || !w[9] || w[8] > 16384 || w[9] > 16384 || (w[6] & ~kSceneKnownFlags)) return false;
  if (w[0] < 4 && (w[18] || w[19] || (w[6] & kSceneNoFrameGeneration))) return false;  // Reserved in 3.
  if (bool(w[19] & 0xFFFF) != bool(w[19] >> 16)) return false;  // Frame size: both or neither.
  const float delta = std::bit_cast<float>(w[10]), locked = std::bit_cast<float>(w[13]);
  const float meters = std::bit_cast<float>(w[15]);
  if (!std::isfinite(delta) || delta < 0 || !std::isfinite(locked) || locked < 0 ||
      !std::isfinite(meters) || meters < 0) return false;
  if ((w[6] & kSceneCamera) && w[1] != uint32_t(ScenePoint::kView)) return false;
  if (w[6] & kSceneCamera)
    for (size_t i = 20; i < kSceneWords; ++i)
      if (!std::isfinite(std::bit_cast<float>(w[i]))) return false;
  m.point = ScenePoint(w[1]);
  m.frame = uint64_t(w[2]) | uint64_t(w[3]) << 32;
  m.session = uint64_t(w[4]) | uint64_t(w[5]) << 32;
  m.flags = w[6]; m.view_id = w[7]; m.width = w[8]; m.height = w[9];
  m.delta_time = delta; m.reset_reason = w[11]; m.camera_register = w[12]; m.locked_distance = locked;
  m.meters_per_unit = meters; m.device = w[14]; m.source_hash = uint64_t(w[16]) | uint64_t(w[17]) << 32;
  m.frame_x = uint16_t(w[18]); m.frame_y = uint16_t(w[18] >> 16);
  m.frame_width = uint16_t(w[19]); m.frame_height = uint16_t(w[19] >> 16);
  if (m.point == ScenePoint::kLockedVolume) {
    for (size_t i = 0; i < 6; ++i) {
      m.locked_box[i] = std::bit_cast<float>(w[20 + i]);
      if (!std::isfinite(m.locked_box[i])) return false;
    }
    for (size_t i = 0; i < 3; ++i)
      if (m.locked_box[i] > m.locked_box[3 + i]) return false;
    return true;
  }
  for (size_t i = 0; i < 12; ++i) m.camera.view[i] = std::bit_cast<float>(w[20 + i]);
  for (size_t i = 0; i < 16; ++i) m.camera.projection[i] = std::bit_cast<float>(w[32 + i]);
  for (size_t i = 0; i < 16; ++i) m.camera.view_projection[i] = std::bit_cast<float>(w[48 + i]);
  return true;
}

inline constexpr uint32_t SceneHeader() {
  return 0xC0000000u | (uint32_t(kSceneWords - 1) << 16) | (kSceneOpcode << 8);
}
// views_accepted: kView markers the backend verified and used;
// last_view_source: the source_hash of the last one (a producer can learn
// which of its candidate shaders carry the camera).
struct SceneStats {
  uint64_t consumed = 0, completed = 0, failed = 0, views_accepted = 0, last_view_source = 0;
};
}  // namespace rex::graphics::title
