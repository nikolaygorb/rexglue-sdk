// Scene history math for the native scene consumer: camera derivation, jitter,
// reset policy and the folded current->previous reprojection. Title-agnostic.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>

namespace rex::graphics::native::scene {
using Matrix = std::array<float, 16>;  // Row-major, column vectors.
using DoubleMatrix = std::array<double, 16>;
inline Matrix Identity() {
  Matrix m{}; for (size_t i = 0; i < 4; ++i) m[i * 5] = 1.f; return m;
}
inline bool InvertDouble(const Matrix& input, DoubleMatrix& output) {
  double a[4][8]{};
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
    if (!std::isfinite(input[y * 4 + x])) return false;
    a[y][x] = input[y * 4 + x]; a[y][x + 4] = x == y ? 1. : 0.;
  }
  for (size_t i = 0; i < 4; ++i) {
    size_t pivot = i;
    for (size_t j = i + 1; j < 4; ++j) if (std::abs(a[j][i]) > std::abs(a[pivot][i])) pivot = j;
    if (std::abs(a[pivot][i]) < 1e-12) return false;
    if (pivot != i) for (size_t x = 0; x < 8; ++x) std::swap(a[pivot][x], a[i][x]);
    const double scale = a[i][i]; for (double& value : a[i]) value /= scale;
    for (size_t j = 0; j < 4; ++j) if (j != i) {
      const double factor = a[j][i];
      for (size_t x = 0; x < 8; ++x) a[j][x] -= factor * a[i][x];
    }
  }
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
    output[y * 4 + x] = a[y][x + 4];
    if (!std::isfinite(output[y * 4 + x])) return false;
  }
  return true;
}
inline bool Invert(const Matrix& input, Matrix& output) {
  DoubleMatrix inverse;
  if (!InvertDouble(input, inverse)) return false;
  for (size_t i = 0; i < output.size(); ++i) {
    output[i] = float(inverse[i]);
    if (!std::isfinite(output[i])) return false;
  }
  return true;
}
inline Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix m{};
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x)
    for (size_t k = 0; k < 4; ++k) m[y * 4 + x] += a[y * 4 + k] * b[k * 4 + x];
  return m;
}
inline DoubleMatrix MultiplyDouble(const DoubleMatrix& a, const DoubleMatrix& b) {
  DoubleMatrix m{};
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x)
    for (size_t k = 0; k < 4; ++k) m[y * 4 + x] += a[y * 4 + k] * b[k * 4 + x];
  return m;
}
// Projection * [view; 0 0 0 1] equals view_projection within a relative tolerance.
inline bool CameraConsistent(const std::array<float, 12>& view, const Matrix& projection, const Matrix& pv) {
  double scale = 0, error = 0;
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
    double value = projection[y * 4 + 3] * (x == 3 ? 1. : 0.);
    for (size_t k = 0; k < 3; ++k) value += double(projection[y * 4 + k]) * view[k * 4 + x];
    scale = std::max(scale, std::abs(double(pv[y * 4 + x])));
    error = std::max(error, std::abs(value - pv[y * 4 + x]));
  }
  return scale > 0 && error <= 1e-3 * std::max(1., scale);
}

// Halton (2, 3) sub-pixel jitter in [-0.5, 0.5) render pixels; phase 0 = first sample.
inline float Halton(uint32_t index, uint32_t base) {
  float f = 1, r = 0;
  for (; index; index /= base) { f /= float(base); r += f * float(index % base); }
  return r;
}
inline std::array<float, 2> Jitter(uint64_t frame, uint32_t phases) {
  if (!phases) return {0, 0};
  const uint32_t i = uint32_t(frame % phases) + 1;
  return {Halton(i, 2) - .5f, Halton(i, 3) - .5f};
}

struct View {
  uint64_t session = 0, frame = 0, color_id = 0, depth_id = 0;
  uint32_t width = 0, height = 0, guest_width = 0, guest_height = 0, scale4 = 0;  // width/height: host pixels.
  // The scene viewport's origin in its target (letterboxed scenes): host, guest pixels.
  uint32_t origin_x = 0, origin_y = 0, guest_origin_x = 0, guest_origin_y = 0;
  uint32_t color_format = 0, depth_format = 0, view_id = 0, title_reset_reason = 0;
  bool title_reset = false, auto_opaque = false;
  float delta_time = 0, locked_distance = 0, meters_per_unit = 0;
  Matrix pv{}, projection{};
  std::array<float, 12> view{};
  std::array<float, 3> ndc_scale{}, ndc_offset{};  // Host NDC mapping without jitter.
  std::array<float, 2> depth_range{};
  // Applied to this frame's scene draws, host pixels: content moved +x right, +y down.
  std::array<float, 2> jitter{};
  // Title kLockedVolume boxes (view space min xyz, max xyz).
  std::array<std::array<float, 6>, 4> locked_boxes{};
  uint32_t locked_count = 0;
};

// Provider-facing camera facts derived from the projection and host mapping.
struct CameraInfo {
  bool valid = false, depth_inverted = false, infinite_far = false;
  bool ndc_inverted = false;  // Projection z/w is 1 at near (reversed-Z), independent of the host mapping.
  double near_distance = 0, far_distance = 0, fov_y = 0, aspect = 0;
  float near_host_depth = 0, far_host_depth = 0;
};
inline double HostDepth(const View& v, double ndc_z) {
  return v.depth_range[0] + (double(v.depth_range[1]) - v.depth_range[0]) * (ndc_z * v.ndc_scale[2] + v.ndc_offset[2]);
}
// Guest clip z/w of a view-space point at distance d in front of the camera.
inline bool NdcAtDistance(const Matrix& p, double distance, double& ndc) {
  const double forward = p[14] >= 0 ? 1. : -1., z = forward * distance;
  const double w = p[14] * z + p[15];
  if (!(w > 0)) return false;
  ndc = (p[10] * z + p[11]) / w;
  return std::isfinite(ndc);
}
inline CameraInfo DeriveCamera(const View& v) {
  CameraInfo info;
  const auto& p = v.projection;
  // Perspective only: w depends on view z, x/y scales present.
  if (std::abs(p[14]) < 1e-9 || std::abs(p[0]) < 1e-12 || std::abs(p[5]) < 1e-12) {
    info.far_host_depth = float(HostDepth(v, 1.));  // Assume Direct3D far at z/w = 1.
    return info;
  }
  auto distance_at = [&](double t, double& out) {  // View distance where clip z/w = t.
    const double denominator = p[10] - t * p[14];
    if (std::abs(denominator) < 1e-12 * std::max(1., std::abs(double(p[10])))) return false;
    out = std::abs((t * p[15] - p[11]) / denominator);
    return std::isfinite(out);
  };
  double d0 = 0, d1 = 0;
  const bool has0 = distance_at(0., d0), has1 = distance_at(1., d1);
  if (!has0 && !has1) { info.far_host_depth = float(HostDepth(v, 1.)); return info; }
  const bool zero_is_near = has0 && (!has1 || d0 < d1);
  info.infinite_far = !(has0 && has1);
  info.near_distance = zero_is_near ? d0 : d1;
  info.far_distance = info.infinite_far ? INFINITY : (zero_is_near ? d1 : d0);
  info.near_host_depth = float(HostDepth(v, zero_is_near ? 0. : 1.));
  info.far_host_depth = float(HostDepth(v, zero_is_near ? 1. : 0.));
  info.depth_inverted = info.near_host_depth > info.far_host_depth;
  info.ndc_inverted = !zero_is_near;
  info.fov_y = 2. * std::atan(1. / std::abs(double(p[5])));
  info.aspect = std::abs(double(p[5]) / p[0]);
  info.valid = std::isfinite(info.near_distance) && info.near_distance > 0 && std::isfinite(info.fov_y);
  return info;
}

inline const char* ResetReason(const View& current, const View* previous) {
  if (!previous) return "first-view";
  if (current.title_reset) return "title";
  if (current.session != previous->session) return "session";
  if (current.view_id != previous->view_id) return "view";
  if (previous->frame == UINT64_MAX || current.frame != previous->frame + 1) return "frame-gap";
  if (current.scale4 != previous->scale4) return "scale";
  // Checked last: the only reason may be the viewport (letterbox bars moving).
  const bool extent = current.width != previous->width || current.height != previous->height ||
                      current.origin_x != previous->origin_x || current.origin_y != previous->origin_y;
  if (current.color_id != previous->color_id || current.depth_id != previous->depth_id ||
      current.color_format != previous->color_format || current.depth_format != previous->depth_format) return "source";
  if (current.depth_range != previous->depth_range || current.ndc_scale != previous->ndc_scale ||
      current.ndc_offset != previous->ndc_offset) return "coordinate-mapping";
  // Conservative fallback for titles without cut events: orientation jump over 45 degrees.
  for (size_t y = 0; y < 3; ++y) {
    double dot = 0, a = 0, b = 0;
    for (size_t x = 0; x < 3; ++x) {
      const auto c = current.view[y * 4 + x], p = previous->view[y * 4 + x];
      dot += double(c) * p; a += double(c) * c; b += double(p) * p;
    }
    if (a < 1e-12 || b < 1e-12 || !std::isfinite(dot) || dot < .70710678 * std::sqrt(a * b)) return "orientation-cut";
  }
  return extent ? "extent" : nullptr;
}
// "extent" alone (the scene viewport moved or resized at the same scale): the
// reprojection maps each frame's own viewport, so motion keeps its history;
// consumers bound to the image size (temporal upscalers) start anew.
inline bool ExtentOnly(const char* reset_reason) {
  return reset_reason && reset_reason[0] == 'e' && std::char_traits<char>::compare(reset_reason, "extent", 7) == 0;
}
// A slot may be recycled only after all its recorded readers finish. The
// previous frame remains pinned even after completion for the next dispatch.
inline size_t FreeSlot(std::span<const uint64_t> retire, size_t previous, uint64_t completed) {
  for (size_t i = 0; i < retire.size(); ++i) if (i != previous && retire[i] <= completed) return i;
  return retire.size();
}

// (Pixel uv, host depth, 1) of a view's frame (with its jitter) -> view-space
// position * w, column-major (the scene pass's locked volumes).
inline bool PixelToView(const View& v, std::array<float, 16>& column_major) {
  const double sx = v.ndc_scale[0], sy = v.ndc_scale[1], sz = v.ndc_scale[2];
  if (std::abs(sx) < 1e-12 || std::abs(sy) < 1e-12 || std::abs(sz) < 1e-12 || !v.width || !v.height) return false;
  const double ox = v.ndc_offset[0] + 2. * v.jitter[0] / v.width;
  const double oy = v.ndc_offset[1] + 2. * v.jitter[1] / v.height;
  const double oz = v.ndc_offset[2], d0 = v.depth_range[0], dd = double(v.depth_range[1]) - d0;
  if (std::abs(dd) < 1e-12) return false;
  const DoubleMatrix to_ndc = {2. / sx, 0, 0, (-1. - ox) / sx,
                               0, 2. / sy, 0, (-1. - oy) / sy,
                               0, 0, 1. / (dd * sz), (-d0 / dd - oz) / sz,
                               0, 0, 0, 1};
  DoubleMatrix inverse_projection;
  if (!InvertDouble(v.projection, inverse_projection)) return false;
  const auto full = MultiplyDouble(inverse_projection, to_ndc);
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
    const float value = float(full[y * 4 + x]);
    if (!std::isfinite(value)) return false;
    column_major[x * 4 + y] = value;
  }
  return true;
}

// Uniform block of the scene pass's locked volumes (std140, one per slot).
struct LockedVolumes {
  std::array<float, 16> pixel_to_view{};
  std::array<float, 4> count{};
  std::array<std::array<float, 4>, 4> box_min{}, box_max{};
};
static_assert(sizeof(LockedVolumes) == 208);

// Push constants of the scene compute pass (<= 128 bytes, the Vulkan minimum).
struct ReprojectionConstants {
  // (pixel uv, host depth, 1) of the current frame -> (previous uv, expected
  // previous host depth, 1) * w, column-major. Zero = no history.
  std::array<float, 16> current_to_previous{};
  std::array<float, 4> jitter{};  // Current jitter in uv xy, previous jitter in uv xy.
  std::array<float, 4> depth{};   // Previous host depth min, max; camera-locked threshold; far host depth.
  std::array<float, 4> options{}; // Reactive scale, reactive cutoff, opaque snapshot present, debug view.
  std::array<float, 4> ndc_depth{};  // Host depth -> guest clip z/w: z * x + y (provider depth input).
};
static_assert(sizeof(ReprojectionConstants) == 128);

// Camera-locked threshold in host depth; equals far (disabled) when unknown.
inline float LockedHostDepth(const View& view, const CameraInfo& camera) {
  double ndc = 0;
  if (!camera.valid || !(view.locked_distance > 0) || !NdcAtDistance(view.projection, view.locked_distance, ndc))
    return camera.far_host_depth;
  return float(HostDepth(view, ndc));
}

inline bool MakeReprojection(const View& current, const View& previous, bool history, ReprojectionConstants& constants) {
  for (const View* v : {&current, &previous}) {
    for (size_t i = 0; i < 3; ++i)
      if (!std::isfinite(v->ndc_scale[i]) || !std::isfinite(v->ndc_offset[i]) || std::abs(v->ndc_scale[i]) < 1e-12f) return false;
    if (!std::isfinite(v->depth_range[0]) || !std::isfinite(v->depth_range[1]) ||
        std::abs(v->depth_range[1] - v->depth_range[0]) < 1e-12f || !v->width || !v->height) return false;
  }
  const auto camera = DeriveCamera(current);
  constants.jitter = {current.jitter[0] / float(current.width), current.jitter[1] / float(current.height),
                      previous.jitter[0] / float(previous.width), previous.jitter[1] / float(previous.height)};
  constants.depth = {std::min(previous.depth_range[0], previous.depth_range[1]),
                     std::max(previous.depth_range[0], previous.depth_range[1]),
                     LockedHostDepth(current, camera), camera.far_host_depth};
  {
    const double dd = double(current.depth_range[1]) - current.depth_range[0];
    constants.ndc_depth = {float(1. / (dd * current.ndc_scale[2])),
                           float((-current.depth_range[0] / dd - current.ndc_offset[2]) / current.ndc_scale[2]), 0, 0};
  }
  constants.current_to_previous = {};
  if (!history) return true;
  // Pixel uv + host depth -> current guest NDC. The current mapping includes
  // the applied jitter: the stored depth was rasterized with it.
  const double sx = current.ndc_scale[0], sy = current.ndc_scale[1], sz = current.ndc_scale[2];
  const double ox = current.ndc_offset[0] + 2. * current.jitter[0] / current.width;
  const double oy = current.ndc_offset[1] + 2. * current.jitter[1] / current.height;
  const double oz = current.ndc_offset[2], d0 = current.depth_range[0], dd = double(current.depth_range[1]) - d0;
  const DoubleMatrix to_ndc = {2. / sx, 0, 0, (-1. - ox) / sx,
                               0, 2. / sy, 0, (-1. - oy) / sy,
                               0, 0, 1. / (dd * sz), (-d0 / dd - oz) / sz,
                               0, 0, 0, 1};
  // Keep the relative transform in double: rounding the world-space inverse
  // first loses cancellation at large world coordinates (false motion).
  DoubleMatrix relative{};
  if (current.pv == previous.pv) {
    for (size_t i = 0; i < 4; ++i) relative[i * 5] = 1.;
  } else {
    DoubleMatrix inverse, prev{};
    if (!InvertDouble(current.pv, inverse)) return false;
    for (size_t i = 0; i < 16; ++i) prev[i] = previous.pv[i];
    relative = MultiplyDouble(prev, inverse);
  }
  // Previous guest clip -> previous unjittered uv and host depth, times w.
  const double psx = previous.ndc_scale[0], psy = previous.ndc_scale[1], psz = previous.ndc_scale[2];
  const double pox = previous.ndc_offset[0], poy = previous.ndc_offset[1], poz = previous.ndc_offset[2];
  const double pd0 = previous.depth_range[0], pdd = double(previous.depth_range[1]) - pd0;
  // uv = (ndc * scale + offset) / 2 + 1/2 after the divide: offsets multiply clip w.
  const std::array<double, 3> scale = {.5 * psx, .5 * psy, pdd * psz};
  const std::array<double, 3> offset = {.5 * pox + .5, .5 * poy + .5, pd0 + pdd * poz};
  DoubleMatrix uv_rows{};
  for (size_t y = 0; y < 3; ++y)
    for (size_t x = 0; x < 4; ++x) uv_rows[y * 4 + x] = scale[y] * relative[y * 4 + x] + offset[y] * relative[12 + x];
  for (size_t x = 0; x < 4; ++x) uv_rows[12 + x] = relative[12 + x];
  const auto full = MultiplyDouble(uv_rows, to_ndc);
  for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
    const float value = float(full[y * 4 + x]);
    if (!std::isfinite(value)) return false;
    constants.current_to_previous[x * 4 + y] = value;
  }
  return true;
}
}  // namespace rex::graphics::native::scene
