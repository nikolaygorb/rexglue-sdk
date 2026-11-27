#include <rex/graphics/native/scene_frames.h>
#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/render_targets.h>
#include <rex/graphics/native/submission.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/ui/vulkan/util.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <fmt/format.h>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>

REXCVAR_DEFINE_BOOL(native_scene_frames, false, "GPU",
    "Retain owned scene color/depth history with camera motion and masks for a temporal consumer. "
    "Needs a title scene producer; on by itself while native_temporal_upscaler or native_frame_generation "
    "is not off. Experimental; object motion: native_scene_object_motion.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_scene_debug, 0, "GPU",
    "Scene consumer view before the HUD: 0 original, 1 retained color, 2 reprojected history "
    "(uncovered tinted red), 3 motion (gray = still), 4 masks (red reactive, green camera-locked, blue sky), "
    "5 object motion (green: pixels with their object's own motion).")
    .range(0, 5).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_scene_jitter, 0, "GPU",
    "Sub-pixel Halton jitter of the title's scene draws for a temporal consumer: phase count, 0 off "
    "(8 for native-resolution AA). Only draws into the verified scene targets and viewport move.")
    .range(0, 64).lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(native_scene_frame_images, false, "GPU",
    "Requested scene captures: export retained GPU color/depth/camera motion/reprojection. Synchronous diagnostics.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {
bool FrameGenerationRequested() {
  // The presenter's setting (another module), fixed for the session.
  static const bool requested = [] {
    const auto value = rex::cvar::GetFlagByName("native_frame_generation");
    return !value.empty() && value != "off";
  }();
  return requested;
}
bool SceneFramesEnabled() {
  return REXCVAR_GET(native_scene_frames) || TemporalUpscalerOn() ||
         FrameGenerationRequested();
}
void WriteUpscalerStatus(Submitter& submitter, const std::string& text) {
  static std::string written;  // The command processor's thread only.
  if (text == written) return;
  written = text;
  std::error_code error;
  const auto directory = rex::filesystem::GetExecutableFolder() / "logs";
  std::filesystem::create_directories(directory, error);
  std::ofstream file(directory / "upscaler.log", std::ios::trunc);
  const auto* device = submitter.vulkan_device();
  file << text;
  if (device && device->properties().deviceName[0]) file << ", " << device->properties().deviceName;
  file << '\n';
}
namespace shaders {
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
}
namespace {
constexpr uint64_t kBudget = uint64_t(768) << 20;
template <class T> void JsonArray(std::ostream& file, const T& values) {
  file << '['; bool first = true;
  for (const auto value : values) { if (!first) file << ','; first = false; file << value; }
  file << ']';
}
uint32_t BytesPerPixel(VkFormat format) {
  return format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : 8;
}
VkFormat ImageFormat(size_t image) {
  return image == SceneFrames::kDepth ? VK_FORMAT_R32_SFLOAT :
         image == SceneFrames::kMasks ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R16G16B16A16_SFLOAT;
}
constexpr uint64_t kBytesPerSlotPixel = 8 + 4 + 8 + 8 + 4;
// Reactive mask from |final - opaque|, as FidelityFX's generator: scale, then cap.
constexpr float kReactiveScale = 1.f, kReactiveCutoff = .9f;
constexpr char kPresentShader[] = R"glsl(#version 460
layout(binding=0) uniform sampler2D scene_color;
layout(push_constant) uniform Constants { vec2 inverse_output_size; vec2 origin; } c;
layout(location=0) out vec4 color;
void main() { color=textureLod(scene_color,(gl_FragCoord.xy-c.origin)*c.inverse_output_size,0.0); }
)glsl";
// Motion: current -> previous in image pixels (the view in the image's corner;
// the previous view may have had another size), without jitter (the convention
// of FSR/XeSS/DLSS); z expected previous host depth; w history coverage. The
// whole image is written: beyond the view its edge repeats (the upscalers take
// the image's constant size, so letterbox steps don't re-create them).
constexpr char kShader[] = R"glsl(#version 460
layout(local_size_x=8, local_size_y=8) in;
layout(binding=0) uniform sampler2D current_color;
layout(binding=1) uniform sampler2D current_depth;
layout(binding=2) uniform sampler2D previous_color;
layout(binding=3) uniform sampler2D previous_depth;
layout(binding=4) uniform sampler2D opaque_color;
layout(binding=5, rgba16f) uniform writeonly image2D motion_image;
layout(binding=6, rgba16f) uniform writeonly image2D debug_image;
layout(binding=7, rgba8) uniform writeonly image2D mask_image;
layout(binding=8, r32f) uniform writeonly image2D ndc_depth_image;
layout(binding=10) uniform sampler2D object_motion;  // previous uv, previous depth, 1 (ObjectMotion)
layout(binding=9, std140) uniform Locked {
  mat4 pixel_to_view;  // (uv, host depth, 1) -> view space * w
  vec4 count;          // x: boxes; yz: previous view / previous image size (its uv in the image)
  vec4 box_min[4];
  vec4 box_max[4];
} l;
layout(push_constant) uniform Constants {
  mat4 current_to_previous; // (uv, host depth, 1) -> (previous uv, expected depth, 1) * w
  vec4 jitter;  // current uv xy, previous uv xy
  vec4 depth;   // previous min, max; camera-locked threshold; far
  vec4 options; // reactive scale, cutoff, flags (1 opaque, 2 history, 4 object motion), debug view
  vec4 ndc_depth; // host depth -> guest clip z/w (provider depth); zw the view's size
} c;
bool IsFar(float d) { return abs(d-c.depth.w)<=1e-6*max(1.0,abs(c.depth.w)); }
void main() {
  // The view's size; the images keep the largest view (letterbox steps fit).
  ivec2 pixel=ivec2(gl_GlobalInvocationID.xy), size=ivec2(c.ndc_depth.zw);
  if (any(greaterThanEqual(pixel,imageSize(motion_image)))) return;
  ivec2 source=min(pixel,size-1);  // Beyond the view: its edge.
  vec2 uv=(vec2(source)+0.5)/vec2(size);
  vec2 previous_size=l.count.yz*vec2(textureSize(previous_color,0));
  vec3 color=texelFetch(current_color,source,0).rgb;
  float depth=texelFetch(current_depth,source,0).r;
  bool has_opaque=mod(c.options.z,2.0)>=1.0, history=mod(floor(c.options.z*0.5),2.0)>=1.0;
  bool objects=c.options.z>=4.0, object_used=false;
  bool sky=IsFar(depth);
  bool locked=!sky && (depth-c.depth.z)*(c.depth.z-c.depth.w)>0.0;
  // Title boxes that move with the camera (a followed third-person character).
  if (!sky && !locked && l.count.x>0.5) {
    vec4 v=l.pixel_to_view*vec4(uv,depth,1.0);
    if (abs(v.w)>1e-12) {
      vec3 p=v.xyz/v.w;
      for (int i=0;i<4;++i)
        if (float(i)<l.count.x && all(greaterThanEqual(p,l.box_min[i].xyz)) && all(lessThanEqual(p,l.box_max[i].xyz)))
          locked=true;
    }
  }
  vec2 motion=vec2(0.0); float expected=0.0, coverage=0.0;
  vec3 previous=color*0.25+vec3(0.25,0.0,0.0);
  if (history && locked) {
    // Camera-locked (first-person weapon): it stays on screen.
    coverage=1.0; previous=textureLod(previous_color,(uv-c.jitter.xy+c.jitter.zw)*l.count.yz,0.0).rgb;
  } else if (history) {
    // The surface's own motion where its object was drawn again (moving,
    // animated), else the camera's.
    vec4 object=objects ? texelFetch(object_motion,source,0) : vec4(0.0);
    // The far plane (no geometry wrote depth there) may be infinitely far (w 0):
    // a point just before it, so its motion is the camera's rotation.
    float motion_depth=sky ? c.depth.w+(c.depth.w<0.5 ? 1e-5 : -1e-5) : depth;
    vec4 p=c.current_to_previous*vec4(uv,motion_depth,1.0);
    bool have=p.w>0.0 && !any(isnan(p)) && !any(isinf(p));
    vec2 previous_uv=have ? p.xy/p.w : vec2(-1.0); if (have) expected=p.z/p.w;
    if (!sky && object.w>0.5) { previous_uv=object.xy; expected=object.z; have=true; object_used=true; }
    if (have) {
      // Also off the previous frame (a fast camera move): the upscalers and frame
      // generation take the real vector there; only the history is uncovered.
      motion=clamp(previous_uv*previous_size-(uv-c.jitter.xy)*vec2(size),vec2(-65504.0),vec2(65504.0));
      if (all(greaterThanEqual(previous_uv,vec2(0.0))) && all(lessThan(previous_uv,vec2(1.0)))) {
        vec2 sample_uv=previous_uv+c.jitter.zw;
        float observed=textureLod(previous_depth,sample_uv*l.count.yz,0.0).r;
        bool valid=sky ? IsFar(observed) :
            (!IsFar(observed) && expected>=c.depth.x-1e-6 && expected<=c.depth.y+1e-6 &&
             abs(observed-expected)<=1e-6+0.01*max(abs(observed),abs(expected)));
        if (valid) { coverage=1.0; previous=textureLod(previous_color,sample_uv*l.count.yz,0.0).rgb; }
      }
    }
  }
  float reactive=0.0;
  if (has_opaque) {
    vec3 d=abs(color-texelFetch(opaque_color,source,0).rgb);
    reactive=min(c.options.y,max(d.r,max(d.g,d.b))*c.options.x);
  }
  vec4 masks=vec4(reactive,locked?1.0:0.0,sky?1.0:0.0,coverage);
  vec4 view=vec4(previous,coverage);
  if (c.options.w==3.0) view=vec4(vec3(clamp(vec2(0.5)+motion/16.0,0.0,1.0),0.5)*(coverage>0.5?1.0:0.4),1.0);
  if (c.options.w==4.0) view=vec4(mix(color*0.3,masks.rgb,0.7),1.0);
  if (c.options.w==5.0) view=vec4(object_used ? mix(color*0.3,vec3(0.0,1.0,0.0),0.6) : color*0.3,1.0);
  imageStore(motion_image,pixel,vec4(motion,expected,coverage));
  imageStore(debug_image,pixel,view);
  imageStore(mask_image,pixel,masks);
  imageStore(ndc_depth_image,pixel,vec4(clamp(depth*c.ndc_depth.x+c.ndc_depth.y,0.0,1.0)));
}
)glsl";
// Frame generation inputs: the scene's motion and depth placed where the scene
// is in the guest frame (outside it: still, far). Optionally a view of them.
struct FrameConstants {
  std::array<float, 4> rect{};   // Scene in the frame, output pixels: x, y, width, height.
  std::array<float, 4> scale{};  // Scene pixels -> frame pixels xy; far depth; view on.
  std::array<int32_t, 4> size{}; // Scene size xy, frame size zw.
  // The previous frame's scene in the frame (the motion's reference; letterbox
  // bars may have moved): motion to its rectangle, output pixels.
  std::array<float, 4> previous_rect{};
  std::array<float, 4> previous_size{};  // The motion's reference view's size, scene pixels.
};
static_assert(sizeof(FrameConstants) == 80);
constexpr char kFrameShader[] = R"glsl(#version 460
layout(local_size_x=8, local_size_y=8) in;
layout(binding=0) uniform sampler2D scene_motion;
layout(binding=1) uniform sampler2D scene_depth;
layout(binding=2) uniform sampler2D hudless;
layout(binding=3, r32f) uniform writeonly image2D frame_depth;
layout(binding=4, rgba16f) uniform writeonly image2D frame_motion;
layout(binding=5, rgba16f) uniform writeonly image2D frame_view;
layout(push_constant) uniform Constants { vec4 rect; vec4 scale; ivec4 size; vec4 previous_rect; vec4 previous_size; } c;
void main() {
  ivec2 pixel=ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(pixel,c.size.zw))) return;
  vec2 local=(vec2(pixel)+0.5-c.rect.xy)/c.rect.zw;
  vec2 motion=vec2(0.0); float depth=c.scale.z;
  bool inside=all(greaterThanEqual(local,vec2(0.0))) && all(lessThan(local,vec2(1.0)));
  if (inside) {
    ivec2 source=clamp(ivec2(local*vec2(c.size.xy)),ivec2(0),c.size.xy-1);
    // Scene pixels -> the previous frame's place in the frame minus this one's
    // (equal rectangles: the scaled scene motion).
    vec2 scene_motion_px=texelFetch(scene_motion,source,0).xy;
    // Image pixels: the previous view had its own size.
    vec2 previous_local=(local*vec2(c.size.xy)+scene_motion_px)/c.previous_size.xy;
    motion=(c.previous_rect.xy+previous_local*c.previous_rect.zw)-(c.rect.xy+local*c.rect.zw);
    depth=texelFetch(scene_depth,source,0).r;
  }
  imageStore(frame_depth,pixel,vec4(depth));
  imageStore(frame_motion,pixel,vec4(motion,0.0,0.0));
  if (c.scale.w>0.5) {
    // Over the frame without the HUD: the scene rectangle must cover the 3D picture exactly.
    vec3 frame=texelFetch(hudless,pixel,0).rgb;
    float near=pow(clamp(abs(depth-c.scale.z),0.0,1.0),0.25);
    vec3 inputs=vec3(clamp(vec2(0.5)+motion/32.0,0.0,1.0),near);
    imageStore(frame_view,pixel,vec4(inside ? mix(frame,inputs,0.6) : frame*0.3,1.0));
  }
}
)glsl";
// World-space camera position and normalized axes from the 3x4 view (world ->
// view, row-major): rows are right, up and back or forward by the projection.
void CameraAxes(const scene::View& view, std::array<float, 3>& position, std::array<float, 3>& up,
                std::array<float, 3>& right, std::array<float, 3>& forward) {
  const auto& v = view.view;
  std::array<std::array<double, 3>, 3> rows{};
  for (size_t y = 0; y < 3; ++y) {
    double length = 0;
    for (size_t x = 0; x < 3; ++x) length += double(v[y * 4 + x]) * v[y * 4 + x];
    length = std::sqrt(length);
    for (size_t x = 0; x < 3; ++x) rows[y][x] = length > 1e-12 ? v[y * 4 + x] / length : 0.;
  }
  // eye = -R^-1 t; R orthogonal up to scale per row.
  std::array<double, 3> eye{};
  for (size_t y = 0; y < 3; ++y) {
    double length2 = 0;
    for (size_t x = 0; x < 3; ++x) length2 += double(v[y * 4 + x]) * v[y * 4 + x];
    if (length2 < 1e-24) continue;
    for (size_t x = 0; x < 3; ++x) eye[x] -= double(v[y * 4 + x]) * v[y * 4 + 3] / length2;
  }
  // Points in front have clip w > 0: view z sign of p[14].
  const double forward_sign = view.projection[14] >= 0 ? 1. : -1.;
  for (size_t x = 0; x < 3; ++x) {
    position[x] = float(eye[x]);
    right[x] = float(rows[0][x]);
    up[x] = float(rows[1][x]);
    forward[x] = float(forward_sign * rows[2][x]);
  }
}
}  // namespace

SceneFrames::~SceneFrames() { Shutdown(); }
bool SceneFrames::Initialize(Submitter& submitter, RenderTargets& targets) {
  submitter_ = &submitter; targets_ = &targets;
  const auto* device = submitter.vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  resolve_ = std::make_unique<ResolvePass>();
  if (!resolve_->Initialize(submitter, targets)) return false;
  std::vector<uint32_t> spirv; std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kShader, spirv, error)) {
    REXGPU_ERROR("Native scene reprojection shader: {}", error); return false;
  }
  shader_ = ui::vulkan::util::CreateShaderModule(device, spirv.data(), spirv.size() * 4);
  if (!shader_) return false;
  std::array<VkDescriptorSetLayoutBinding, 11> bindings{};
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].descriptorType = i < 5 || i == 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                 i < 9 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  }
  VkDescriptorSetLayoutCreateInfo set{}; set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set.bindingCount = uint32_t(bindings.size()); set.pBindings = bindings.data();
  if (fn.vkCreateDescriptorSetLayout(vk, &set, nullptr, &set_layout_) != VK_SUCCESS) return false;
  set.bindingCount = 1;
  auto present_binding = bindings[0]; present_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  set.pBindings = &present_binding;
  if (fn.vkCreateDescriptorSetLayout(vk, &set, nullptr, &present_set_layout_) != VK_SUCCESS) return false;
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(scene::ReprojectionConstants)};
  VkPipelineLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
  if (fn.vkCreatePipelineLayout(vk, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;
  push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; push.size = 16;
  layout.pSetLayouts = &present_set_layout_;
  if (fn.vkCreatePipelineLayout(vk, &layout, nullptr, &present_layout_) != VK_SUCCESS) return false;
  // The linked glslang appends to its output vector; each module owns a fresh one.
  std::vector<uint32_t> present_spirv;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, kPresentShader, present_spirv, error)) {
    REXGPU_ERROR("Native scene output shader: {}", error); return false;
  }
  present_ps_ = ui::vulkan::util::CreateShaderModule(device, present_spirv.data(), present_spirv.size() * 4);
  present_vs_ = ui::vulkan::util::CreateShaderModule(device, shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs));
  if (!present_ps_ || !present_vs_) return false;
  VkComputePipelineCreateInfo pipe{}; pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipe.stage.module = shader_; pipe.stage.pName = "main"; pipe.layout = pipeline_layout_;
  if (fn.vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &pipe, nullptr, &pipeline_) != VK_SUCCESS) return false;
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uint32_t(kSlotCount)}};
  VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = uint32_t(kSlotCount * 3 + 1); pool.poolSizeCount = 3; pool.pPoolSizes = sizes;
  if (fn.vkCreateDescriptorPool(vk, &pool, nullptr, &pool_) != VK_SUCCESS) return false;
  for (auto& slot : slots_) {
    VkDescriptorSetAllocateInfo alloc{}; alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &set_layout_;
    if (fn.vkAllocateDescriptorSets(vk, &alloc, &slot.set) != VK_SUCCESS) return false;
    alloc.pSetLayouts = &present_set_layout_;
    for (auto& present_set : slot.present_sets)
      if (fn.vkAllocateDescriptorSets(vk, &alloc, &present_set) != VK_SUCCESS) return false;
  }
  {
    VkDescriptorSetAllocateInfo alloc{}; alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = pool_; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &present_set_layout_;
    if (fn.vkAllocateDescriptorSets(vk, &alloc, &upscaled_set_) != VK_SUCCESS) return false;
  }
  // Locked volumes: one uniform block per slot, bound once.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(device, kLockedStride * kSlotCount,
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ui::vulkan::util::MemoryPurpose::kUpload, locked_buffer_,
          locked_memory_, &locked_memory_type_, &locked_memory_size_)) return false;
  void* locked_mapping = nullptr;
  if (fn.vkMapMemory(vk, locked_memory_, 0, VK_WHOLE_SIZE, 0, &locked_mapping) != VK_SUCCESS) return false;
  locked_mapping_ = static_cast<uint8_t*>(locked_mapping);
  std::memset(locked_mapping_, 0, size_t(kLockedStride * kSlotCount));
  ui::vulkan::util::FlushMappedMemoryRange(device, locked_memory_, locked_memory_type_, 0, locked_memory_size_);
  for (size_t i = 0; i < slots_.size(); ++i) {
    VkDescriptorBufferInfo info{locked_buffer_, kLockedStride * i, sizeof(scene::LockedVolumes)};
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = slots_[i].set;
    write.dstBinding = 9; write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &info;
    fn.vkUpdateDescriptorSets(vk, 1, &write, 0, nullptr);
  }
  VkSamplerCreateInfo sampler{}; sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(vk, &sampler, nullptr, &linear_) != VK_SUCCESS) return false;
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  return fn.vkCreateSampler(vk, &sampler, nullptr, &nearest_) == VK_SUCCESS;
}
void SceneFrames::DestroyImage(ResolvedTexture& image) {
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  if (image.framebuffer) fn.vkDestroyFramebuffer(vk, image.framebuffer, nullptr);
  if (image.view) fn.vkDestroyImageView(vk, image.view, nullptr);
  if (image.image) fn.vkDestroyImage(vk, image.image, nullptr);
  if (image.memory) fn.vkFreeMemory(vk, image.memory, nullptr);
  image = {};
}
void SceneFrames::Shutdown() {
  if (!submitter_) return;
  // Includes partially recorded failed copies/dispatches, not just committed leases.
  upscaler_.reset();  // Awaits the replayed dispatches that use its context.
  submitter_->AwaitAll();
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  resolve_.reset();
  for (const auto& item : present_framebuffers_) fn.vkDestroyFramebuffer(vk, item.second, nullptr);
  for (const auto& item : present_pipelines_) fn.vkDestroyPipeline(vk, item.second, nullptr);
  for (const auto& item : present_passes_) fn.vkDestroyRenderPass(vk, item.second, nullptr);
  present_framebuffers_.clear(); present_pipelines_.clear(); present_passes_.clear();
  if (pool_) fn.vkDestroyDescriptorPool(vk, pool_, nullptr);
  if (locked_mapping_) fn.vkUnmapMemory(vk, locked_memory_);
  if (locked_buffer_) fn.vkDestroyBuffer(vk, locked_buffer_, nullptr);
  if (locked_memory_) fn.vkFreeMemory(vk, locked_memory_, nullptr);
  locked_mapping_ = nullptr; locked_buffer_ = VK_NULL_HANDLE; locked_memory_ = VK_NULL_HANDLE;
  for (auto& slot : slots_) for (auto& image : slot.images) DestroyImage(image);
  DestroyImage(opaque_); DestroyImage(ndc_depth_); DestroyImage(upscaled_);
  DestroyImage(hudless_); DestroyImage(fg_view_);
  if (fg_pool_) fn.vkDestroyDescriptorPool(vk, fg_pool_, nullptr);
  if (fg_pipeline_) fn.vkDestroyPipeline(vk, fg_pipeline_, nullptr);
  if (fg_pipeline_layout_) fn.vkDestroyPipelineLayout(vk, fg_pipeline_layout_, nullptr);
  if (fg_set_layout_) fn.vkDestroyDescriptorSetLayout(vk, fg_set_layout_, nullptr);
  if (fg_shader_) fn.vkDestroyShaderModule(vk, fg_shader_, nullptr);
  fg_pool_ = VK_NULL_HANDLE; fg_pipeline_ = VK_NULL_HANDLE; fg_pipeline_layout_ = VK_NULL_HANDLE;
  fg_set_layout_ = VK_NULL_HANDLE; fg_shader_ = VK_NULL_HANDLE; fg_sets_ = {}; fg_set_retire_ = {};
  fg_building_ = fg_ended_ = fg_written_frame_ = {};
  upscaled_set_ = VK_NULL_HANDLE; upscaler_attempted_ = upscaler_active_ = false;
  if (linear_) fn.vkDestroySampler(vk, linear_, nullptr);
  if (nearest_) fn.vkDestroySampler(vk, nearest_, nullptr);
  if (pipeline_) fn.vkDestroyPipeline(vk, pipeline_, nullptr);
  if (pipeline_layout_) fn.vkDestroyPipelineLayout(vk, pipeline_layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(vk, set_layout_, nullptr);
  if (shader_) fn.vkDestroyShaderModule(vk, shader_, nullptr);
  if (present_layout_) fn.vkDestroyPipelineLayout(vk, present_layout_, nullptr);
  if (present_set_layout_) fn.vkDestroyDescriptorSetLayout(vk, present_set_layout_, nullptr);
  if (present_vs_) fn.vkDestroyShaderModule(vk, present_vs_, nullptr);
  if (present_ps_) fn.vkDestroyShaderModule(vk, present_ps_, nullptr);
  present_layout_ = VK_NULL_HANDLE; present_set_layout_ = VK_NULL_HANDLE; present_vs_ = present_ps_ = VK_NULL_HANDLE;
  slots_ = {}; pool_ = VK_NULL_HANDLE; linear_ = nearest_ = VK_NULL_HANDLE;
  pipeline_ = VK_NULL_HANDLE; pipeline_layout_ = VK_NULL_HANDLE; set_layout_ = VK_NULL_HANDLE; shader_ = VK_NULL_HANDLE;
  submitter_ = nullptr; targets_ = nullptr; current_ = previous_ = kNoSlot; pending_valid_ = retained_ = opaque_taken_ = false;
}
bool SceneFrames::CreateImage(ResolvedTexture& image, uint32_t width, uint32_t height, VkFormat format, bool storage) {
  image.vk_format = format; image.host_width = width; image.host_height = height; image.attachment = !storage;
  VkFormatFeatureFlags required = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (storage ? VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
  if (format == VK_FORMAT_R16G16B16A16_SFLOAT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((targets_->OptimalTilingFeatures(format) & required) != required || (!storage && !resolve_->CanDrawInto(format))) return false;
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions();
  VkImageCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format; info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      (storage ? VK_IMAGE_USAGE_STORAGE_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal, image.image, image.memory)) return false;
  VkImageViewCreateInfo view{}; view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; view.image = image.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = format; view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
  return fn.vkCreateImageView(device->device(), &view, nullptr, &image.view) == VK_SUCCESS;
}
void SceneFrames::BeginView(const scene::View& view) {
  pending_ = view; pending_valid_ = true; retained_ = images_exported_ = opaque_taken_ = opaque_valid_ = false;
  output_width_ = output_height_ = output_mask_ = 0;
}
bool SceneFrames::SnapshotOpaque(RenderTarget& color) {
  if (!submitter_ || !pending_valid_ || retained_ || opaque_taken_) return false;
  opaque_taken_ = true;  // One boundary per frame, also when the copy fails.
  if (color.depth || color.scale4 != pending_.scale4 ||
      color.layout == VK_IMAGE_LAYOUT_UNDEFINED || pending_.origin_x + pending_.width > color.host_width ||
      pending_.origin_y + pending_.height > color.host_height) return Reject("opaque-source");
  // Grows only: a smaller view (letterbox steps) uses its corner, no GPU wait.
  if (opaque_.host_width < pending_.width || opaque_.host_height < pending_.height) {
    const uint32_t width = std::max(pending_.width, opaque_.host_width);
    const uint32_t height = std::max(pending_.height, opaque_.host_height);
    // The previous frame's dispatch may still read it.
    if (opaque_.image) ++image_growths_;
    if (opaque_.image && !submitter_->Await(opaque_retire_)) { ++failures_; return false; }
    DestroyImage(opaque_);
    if (!CreateImage(opaque_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, false)) {
      DestroyImage(opaque_); ++failures_; return false;
    }
  }
  opaque_retire_ = submitter_->current();
  const Rect rect{0, 0, int32_t(pending_.width), int32_t(pending_.height)};
  if (!resolve_->Draw(color, opaque_, rect, color.fixed16 ? 32.f : 1.f, (1u << uint32_t(color.samples)) - 1u,
                      int32_t(pending_.origin_x), int32_t(pending_.origin_y))) {
    ++failures_; return false;
  }
  opaque_valid_ = true; opaque_color_id_ = color.id; ++opaque_count_; return true;
}
void SceneFrames::InvalidateView() { pending_valid_ = retained_ = false; }
void SceneFrames::AddLockedVolume(const std::array<float, 6>& box) {
  if (!pending_valid_ || retained_ || pending_.locked_count >= pending_.locked_boxes.size()) return;
  pending_.locked_boxes[pending_.locked_count++] = box;
}

bool SceneFrames::Reject(const char* reason) {
  ++rejected_; last_reject_reason_ = reason; last_reject_frame_ = pending_.frame; last_reject_session_ = pending_.session;
  ++reject_reasons_[reason];
  return false;
}
bool SceneFrames::Retain(RenderTarget& color, RenderTarget& depth) {
  // The CPU time of the scene end (GPU waits included), for the report.
  struct Timer {
    SceneFrames& frames;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~Timer() {
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      frames.retain_max_ms_ = std::max(frames.retain_max_ms_, ms);
      if (ms > 4.0) ++frames.retain_slow_;
    }
  } timer{*this};
  const char* reason = !submitter_ || !pending_valid_ ? "retain-no-view" : retained_ ? "retain-duplicate" :
      !pending_.width || !pending_.height ? "retain-empty-extent" :
      depth.id != pending_.depth_id ? "retain-source-id" :
      color.depth || !depth.depth ? "retain-source-type" :
      color.scale4 != pending_.scale4 || depth.scale4 != pending_.scale4 ? "retain-scale" :
      color.layout == VK_IMAGE_LAYOUT_UNDEFINED || depth.layout == VK_IMAGE_LAYOUT_UNDEFINED ? "retain-undefined-layout" :
      pending_.origin_x + pending_.width > color.host_width || pending_.origin_x + pending_.width > depth.host_width ||
      pending_.origin_y + pending_.height > color.host_height ||
      pending_.origin_y + pending_.height > depth.host_height ? "retain-source-extent" : nullptr;
  if (reason) { pending_valid_ = false; return Reject(reason); }
  // The scene colour is what is bound at scene end: titles may shade into
  // another surface than the pass that carried the camera (same depth).
  pending_.color_id = color.id; pending_.color_format = color.format;
  std::array<uint64_t, kSlotCount> retire{};
  for (size_t i = 0; i < slots_.size(); ++i) retire[i] = slots_[i].retire;
  const size_t index = scene::FreeSlot(retire, current_, submitter_->completed());
  if (index == kNoSlot) { ++busy_skips_; pending_valid_ = false; return false; }
  auto& slot = slots_[index];
  // Slot images grow only, to the largest view so far (extent_): a smaller view
  // (letterbox steps) uses their corner without new images.
  extent_guest_width_ = std::max(extent_guest_width_, pending_.guest_width);
  extent_guest_height_ = std::max(extent_guest_height_, pending_.guest_height);
  const uint32_t extent_width = ExtentWidth(pending_), extent_height = ExtentHeight(pending_);
  if (slot.images[0].host_width < extent_width || slot.images[0].host_height < extent_height) {
    uint32_t width = extent_width, height = extent_height;
    for (const auto& other : slots_) {
      width = std::max(width, other.images[0].host_width); height = std::max(height, other.images[0].host_height);
    }
    if (slot.images[0].image) ++image_growths_;
    for (auto& image : slot.images) DestroyImage(image);
    uint64_t allocated = uint64_t(width) * height * kBytesPerSlotPixel;
    for (const auto& other : slots_) for (const auto& image : other.images)
      if (image.image) allocated += uint64_t(image.host_width) * image.host_height * BytesPerPixel(image.vk_format);
    if (allocated > kBudget) { pending_valid_ = false; return Reject("retain-memory-budget"); }
    for (size_t i = 0; i < slot.images.size(); ++i)
      if (!CreateImage(slot.images[i], width, height, ImageFormat(i), i >= kMotion)) {
        for (auto& image : slot.images) DestroyImage(image);
        ++failures_; pending_valid_ = false; return false;
      }
    const auto* device = submitter_->vulkan_device();
    std::array<VkDescriptorImageInfo, 2> images{}; std::array<VkWriteDescriptorSet, 2> writes{};
    for (size_t i = 0; i < 2; ++i) {
      images[i] = {linear_, slot.images[i == 0 ? kColor : kDebug].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = slot.present_sets[i];
      writes[i].descriptorCount = 1; writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[i].pImageInfo = &images[i];
    }
    device->functions().vkUpdateDescriptorSets(device->device(), 2, writes.data(), 0, nullptr);
  }
  slot.view = pending_; slot.lease = ++next_lease_;
  slot.retire = submitter_->current();  // Protect partial recordings too.
  const Rect rect{0, 0, int32_t(pending_.width), int32_t(pending_.height)};
  const int32_t origin_x = int32_t(pending_.origin_x), origin_y = int32_t(pending_.origin_y);
  if (!resolve_->Draw(color, slot.images[0], rect, color.fixed16 ? 32.f : 1.f, (1u << uint32_t(color.samples)) - 1u,
                      origin_x, origin_y) ||
      !resolve_->Draw(depth, slot.images[1], rect, 1.f, 1, origin_x, origin_y)) {
    ++failures_; pending_valid_ = false; return false;
  }
  Slot* previous = current_ < kNoSlot ? &slots_[current_] : nullptr;
  const auto* reset = scene::ResetReason(pending_, previous ? &previous->view : nullptr);
  // Motion (and frame generation) keeps its history across letterbox steps.
  const bool motion_history = !reset || scene::ExtentOnly(reset);
  reset_reason_ = reset ? (motion_history ? "extent (motion history kept)" : reset) : "none";
  previous_lease_ = previous ? previous->lease : 0;
  if (reset && !motion_history) {
    ++resets_; ++reset_reasons_[reset];
    if (previous && logged_resets_ < 32) {
      ++logged_resets_;
      const auto& before = previous->view;
      REXGPU_INFO("Scene history reset at frame {} ({} after frame {}): {}; colour {} format {} -> {} format {}, "
                  "depth {} format {} -> {} format {}, {}x{} -> {}x{}", pending_.frame, pending_.frame - before.frame,
                  before.frame, reset, before.color_id, before.color_format, pending_.color_id, pending_.color_format,
                  before.depth_id, before.depth_format, pending_.depth_id, pending_.depth_format, before.width,
                  before.height, pending_.width, pending_.height);
    }
  }
  if (reset && motion_history) ++extent_continues_;
  if (pending_.title_reset) ++title_resets_;
  if (pending_.jitter[0] || pending_.jitter[1]) ++jittered_;
  bool created = false;
  // Object motion: the captured scene draws against this depth (frames
  // without history only move its frames on).
  bool objects = false;
  if (object_motion_) {
    objects = object_motion_->Record(slot.images[kDepth], pending_.width, pending_.height, depth.samples,
                                     motion_history);
    if (objects) targets_->NoteGraphicsStateChange();
  }
  if (!EnsureShared(ndc_depth_, VK_FORMAT_R32_SFLOAT, true, created, ExtentWidth(pending_),
                    ExtentHeight(pending_)) ||
      !Dispatch(slot, previous, motion_history, objects)) {
    ++failures_; pending_valid_ = false; return false;
  }
  previous_ = current_; current_ = index; retained_ = true; ++retained_count_;
  if (fg_building_.session == pending_.session && fg_building_.frame == pending_.frame) {
    fg_building_.retained = true; fg_building_.slot = index; fg_building_.lease = slot.lease;
    fg_building_.reset = !motion_history;
  }
  // Active (swap_post_effect skipped) until the upscaler is off or skips
  // many frames in a row: a stray skipped frame must not flicker the AA.
  const bool wanted = TemporalUpscalerOn();
  // The upscalers take the largest view's size: letterbox steps keep their history.
  const bool upscaled = wanted && Upscale(color, slot, motion_history);
  upscaler_misses_ = upscaled ? 0 : upscaler_misses_ + 1;
  upscaler_active_ = wanted && upscaler_ && (upscaled || (upscaler_active_ && upscaler_misses_ < 30));
  return true;
}
bool SceneFrames::Dispatch(Slot& current, Slot* previous, bool history, bool objects) {
  if (!previous || !history) { previous = &current; history = false; }
  objects = objects && history && object_motion_ && object_motion_->output().image &&
            object_motion_->output().host_width >= current.view.width &&
            object_motion_->output().host_height >= current.view.height;
  scene::ReprojectionConstants constants{};
  if (!scene::MakeReprojection(current.view, previous->view, history, constants)) return false;
  // The slot's locked volumes (its uniform block; the slot is free of readers).
  if (locked_mapping_) {
    scene::LockedVolumes locked{};
    const auto& view = current.view;
    if (view.locked_count && scene::PixelToView(view, locked.pixel_to_view)) {
      locked.count[0] = float(view.locked_count);
      for (uint32_t i = 0; i < view.locked_count; ++i)
        for (size_t axis = 0; axis < 3; ++axis) {
          locked.box_min[i][axis] = view.locked_boxes[i][axis];
          locked.box_max[i][axis] = view.locked_boxes[i][3 + axis];
        }
      ++locked_frames_;
    }
    // The previous frame's view part of its images (uv scale).
    locked.count[1] = float(previous->view.width) / float(std::max(previous->images[kColor].host_width, 1u));
    locked.count[2] = float(previous->view.height) / float(std::max(previous->images[kColor].host_height, 1u));
    const size_t slot_index = size_t(&current - slots_.data());
    std::memcpy(locked_mapping_ + kLockedStride * slot_index, &locked, sizeof(locked));
    ui::vulkan::util::FlushMappedMemoryRange(submitter_->vulkan_device(), locked_memory_, locked_memory_type_,
                                             kLockedStride * slot_index, locked_memory_size_, kLockedStride);
  }
  // Reactive compares the same surface before and after translucency.
  const bool opaque = opaque_valid_ && opaque_color_id_ == current.view.color_id;
  opaque_valid_ = false;
  const int32_t debug_view = REXCVAR_GET(native_scene_debug);
  constants.ndc_depth[2] = float(current.view.width); constants.ndc_depth[3] = float(current.view.height);
  constants.options = {kReactiveScale, kReactiveCutoff,
                       float((opaque ? 1 : 0) | (history ? 2 : 0) | (objects ? 4 : 0)),
                       float(debug_view >= 3 ? debug_view : 2)};
  auto* commands = submitter_->OpenOutsideRenderPass(); if (!commands) return false;
  current.retire = std::max(current.retire, submitter_->current());
  previous->retire = std::max(previous->retire, submitter_->current());
  if (opaque) opaque_retire_ = submitter_->current();
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions();
  // Binding 10 (here 9): the object motion, else any sampled image (unread).
  std::array<ResolvedTexture*, 10> images = {&current.images[kColor], &current.images[kDepth], &previous->images[kColor],
      &previous->images[kDepth], opaque ? &opaque_ : &current.images[kColor], &current.images[kMotion],
      &current.images[kDebug], &current.images[kMasks], &ndc_depth_,
      objects ? &object_motion_->output() : &current.images[kColor]};
  if (objects) { object_motion_->UseOutput(submitter_->current()); ++object_frames_; }
  std::array<VkDescriptorImageInfo, 10> infos{}; std::array<VkWriteDescriptorSet, 10> writes{};
  for (uint32_t i = 0; i < images.size(); ++i) {
    auto& image = *images[i];
    const bool sampled = i < 5 || i == 9;
    TransitionTrackedImage(fn, commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout,
                           sampled ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL);
    infos[i].imageView = image.view; infos[i].imageLayout = image.layout;
    infos[i].sampler = sampled ? (i == 1 || i == 3 || i == 9 ? nearest_ : linear_) : VK_NULL_HANDLE;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = current.set;
    writes[i].dstBinding = i == 9 ? 10 : i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = sampled ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &infos[i];
  }
  fn.vkUpdateDescriptorSets(device->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  commands->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  commands->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1, &current.set, 0, nullptr);
  commands->CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  commands->CmdVkDispatch((ExtentWidth(current.view) + 7) / 8, (ExtentHeight(current.view) + 7) / 8, 1);
  ++dispatch_count_; return true;
}
VkRenderPass SceneFrames::PresentRenderPass(VkFormat format) {
  if (const auto found = present_passes_.find(format); found != present_passes_.end()) return found->second;
  VkAttachmentDescription attachment{}; attachment.format = format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  // LOAD plus a channel write mask preserves the title's destination alpha.
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
  VkRenderPassCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = 1; info.pAttachments = &attachment; info.subpassCount = 1; info.pSubpasses = &subpass;
  const auto* device = submitter_->vulkan_device(); VkRenderPass pass = VK_NULL_HANDLE;
  if (device->functions().vkCreateRenderPass(device->device(), &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
  present_passes_.emplace(format, pass); return pass;
}
VkPipeline SceneFrames::PresentPipeline(VkFormat format, uint32_t mask) {
  const uint64_t key = uint64_t(uint32_t(format)) << 4 | mask;
  if (const auto found = present_pipelines_.find(key); found != present_pipelines_.end()) return found->second;
  const auto pass = PresentRenderPass(format); if (!pass) return VK_NULL_HANDLE;
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = present_vs_;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = present_ps_;
  VkPipelineVertexInputStateCreateInfo vertex{}; vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo assembly{}; assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{}; viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{}; raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_CLOCKWISE; raster.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo msaa{}; msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{}; blend.colorWriteMask = mask;
  VkPipelineColorBlendStateCreateInfo blending{}; blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blending.attachmentCount = 1; blending.pAttachments = &blend;
  const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{}; dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = 2; info.pStages = stages.data(); info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &msaa;
  info.pColorBlendState = &blending; info.pDynamicState = &dynamic; info.layout = present_layout_; info.renderPass = pass;
  const auto* device = submitter_->vulkan_device(); VkPipeline pipeline = VK_NULL_HANDLE;
  if (device->functions().vkCreateGraphicsPipelines(device->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS) return VK_NULL_HANDLE;
  present_pipelines_.emplace(key, pipeline); return pipeline;
}
bool SceneFrames::PresentDebug(RenderTarget& target, uint32_t width, uint32_t height, uint32_t color_mask) {
  const int mode = REXCVAR_GET(native_scene_debug);
  if (mode < 0 || mode > 5 || color_mask > 15) return false;
  if (!retained_ || current_ >= kNoSlot) return Reject("output-no-retained-frame");
  if (target.depth || target.samples != VK_SAMPLE_COUNT_1_BIT) return Reject("output-format-or-msaa");
  if (!width || !height || width > target.host_width || height > target.host_height) return Reject("output-extent");
  output_width_ = width; output_height_ = height; output_mask_ = color_mask;
  if (!mode) return true;
  auto& slot = slots_[current_];
  slot.retire = std::max(slot.retire, submitter_->current());
  if (!DrawImage(target, slot.images[mode == 1 ? kColor : kDebug], slot.present_sets[mode == 1 ? 0 : 1], width, height,
                 color_mask)) { ++failures_; return false; }
  ++debug_count_; return true;
}
bool SceneFrames::DrawImage(RenderTarget& target, ResolvedTexture& image, VkDescriptorSet set, uint32_t width,
                            uint32_t height, uint32_t color_mask, uint32_t x, uint32_t y) {
  if (x + width > target.host_width || y + height > target.host_height) return false;
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  const auto pass = PresentRenderPass(target.vk_format); const auto pipeline = PresentPipeline(target.vk_format, color_mask);
  if (!pass || !pipeline) return false;
  auto found = present_framebuffers_.find(&target);
  if (found == present_framebuffers_.end()) {
    VkFramebufferCreateInfo info{}; info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass; info.attachmentCount = 1; info.pAttachments = &target.view;
    info.width = target.host_width; info.height = target.host_height; info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (fn.vkCreateFramebuffer(vk, &info, nullptr, &framebuffer) != VK_SUCCESS) return false;
    found = present_framebuffers_.emplace(&target, framebuffer).first;
  }
  auto* commands = submitter_->OpenOutsideRenderPass(); if (!commands) return false;
  TransitionTrackedImage(fn, commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  targets_->Transition(commands, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderPassBeginInfo begin{}; begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = pass; begin.framebuffer = found->second;
  begin.renderArea.offset = {int32_t(x), int32_t(y)}; begin.renderArea.extent = {width, height};
  commands->CmdVkBeginRenderPass(&begin, VK_SUBPASS_CONTENTS_INLINE);
  commands->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  VkViewport viewport{}; viewport.x = float(x); viewport.y = float(y);
  viewport.width = float(width); viewport.height = float(height); viewport.maxDepth = 1.f;
  VkRect2D scissor{{int32_t(x), int32_t(y)}, {width, height}};
  commands->CmdVkSetViewport(0, 1, &viewport); commands->CmdVkSetScissor(0, 1, &scissor);
  commands->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout_, 0, 1, &set, 0, nullptr);
  // Texels 1:1: the image may be larger than the part drawn (it keeps the largest view).
  const float constants[]{1.f / float(image.host_width), 1.f / float(image.host_height), float(x), float(y)};
  commands->CmdVkPushConstants(present_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
  commands->CmdVkDraw(3, 1, 0, 0); commands->CmdVkEndRenderPass(); targets_->NoteGraphicsStateChange();
  return true;
}
bool SceneFrames::EnsureShared(ResolvedTexture& image, VkFormat format, bool storage, bool& created,
                               uint32_t width, uint32_t height) {
  created = false;
  if (!width || !height) { width = pending_.width; height = pending_.height; }
  // Grows only: smaller sizes (letterbox steps) use its corner, so only a new
  // largest size waits for the GPU (earlier frames may still use the image).
  if (image.image && image.host_width >= width && image.host_height >= height) return true;
  if (image.image) {
    width = std::max(width, image.host_width); height = std::max(height, image.host_height);
    ++image_growths_;
    if (!submitter_->AwaitAll()) return false;
  }
  DestroyImage(image);
  if (!CreateImage(image, width, height, format, storage)) { DestroyImage(image); return false; }
  created = true; return true;
}
uint32_t SceneFrames::ExtentWidth(const scene::View& view) const {
  return std::max(ScaleSize(extent_guest_width_, view.scale4), view.width);
}
uint32_t SceneFrames::ExtentHeight(const scene::View& view) const {
  return std::max(ScaleSize(extent_guest_height_, view.scale4), view.height);
}
void SceneFrames::PadColor(CommandRecorder* commands, Slot& slot) {
  auto& image = slot.images[kColor];
  const uint32_t width = slot.view.width, height = slot.view.height;
  const uint32_t extent_width = std::min(ExtentWidth(slot.view), image.host_width);
  const uint32_t extent_height = std::min(ExtentHeight(slot.view), image.host_height);
  if ((extent_width <= width && extent_height <= height) || !width || !height) return;
  constexpr VkFormatFeatureFlags kBlit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
  if ((targets_->OptimalTilingFeatures(image.vk_format) & kBlit) != kBlit) return;  // Then stale beyond the view.
  const auto& fn = submitter_->vulkan_device()->functions();
  TransitionTrackedImage(fn, commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, VK_IMAGE_LAYOUT_GENERAL);
  VkImageBlit blit{};
  blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
  // The last column to the right, then the last row (with that) down: the
  // upscalers see the view's edge, not older frames, beyond it.
  if (extent_width > width) {
    blit.srcOffsets[0] = {int32_t(width) - 1, 0, 0}; blit.srcOffsets[1] = {int32_t(width), int32_t(height), 1};
    blit.dstOffsets[0] = {int32_t(width), 0, 0}; blit.dstOffsets[1] = {int32_t(extent_width), int32_t(height), 1};
    commands->CmdVkBlitImage(image.image, VK_IMAGE_LAYOUT_GENERAL, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                             VK_FILTER_NEAREST);
  }
  if (extent_height > height) {
    if (extent_width > width)
      ImageBarrier(fn, commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    blit.srcOffsets[0] = {0, int32_t(height) - 1, 0}; blit.srcOffsets[1] = {int32_t(extent_width), int32_t(height), 1};
    blit.dstOffsets[0] = {0, int32_t(height), 0}; blit.dstOffsets[1] = {int32_t(extent_width), int32_t(extent_height), 1};
    commands->CmdVkBlitImage(image.image, VK_IMAGE_LAYOUT_GENERAL, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                             VK_FILTER_NEAREST);
  }
}
uint32_t SceneFrames::UpscalerJitterPhases() {
  if (!TemporalUpscalerOn() || !upscaler_ || upscaler_->failed() || current_ >= kNoSlot) return 0;
  const auto& view = slots_[current_].view;
  // Display: the scale's size when the scene was drawn below it (native_scene_upscale).
  return upscaler_->JitterPhases(view.width, ScaleSize(view.guest_width, targets_->scale4()));
}
bool SceneFrames::UpscaleFits() const {
  // The scene's own scale (1x) to the output's.
  return UpscaleWithin(upscaler_ ? upscaler_->MaxUpscaleRatio() : 0.0, targets_->scale4(), targets_->scene_scale4());
}
bool SceneFrames::Upscale(RenderTarget& target, Slot& slot, bool history) {
  // The provider (AMD FSR or Intel XeSS) switches while running: the other one
  // is released (after its GPU work) and the wanted one loaded once.
  const UpscalerProvider provider = TemporalUpscalerProvider();
  if (provider != upscaler_provider_) {
    upscaler_.reset();
    upscaler_provider_ = provider; upscaler_attempted_ = false;
  }
  const std::string name = UpscalerName(provider);
  if (!upscaler_) {
    if (upscaler_attempted_) return false;
    upscaler_attempted_ = true;
    std::string reason;
    upscaler_ = TemporalUpscaler::Load(provider, *submitter_, reason);
    if (!upscaler_) {
      upscaler_state_ = "unavailable: " + reason;
      WriteUpscalerStatus(*submitter_, name + " unavailable: " + reason);
      REXGPU_WARN("Temporal upscaler: {} unavailable, the original image is kept: {}", name, reason);
      return false;
    }
    upscaler_state_ = "loaded";
  }
  upscaler_->SetWantedMajor(TemporalUpscalerMajor());  // fsr3 / fsr4, switchable while running.
  if (upscaler_->failed()) {
    const std::string error = upscaler_->last_error().empty() ? std::string("dispatch") : upscaler_->last_error();
    REXGPU_ERROR("Temporal upscaler: {} failed ({}); disabled, the original image is kept", name, error);
    upscaler_state_ = "failed: " + error; upscaler_.reset();
    WriteUpscalerStatus(*submitter_, name + " failed (" + error + ")");
    return false;
  }
  // native_scene_upscale above the provider's ratio: native-resolution AA instead.
  const bool limited = REXCVAR_GET(native_scene_upscale) && !UpscaleFits();
  if (limited != upscale_limited_) {
    upscale_limited_ = limited;
    if (limited)
      REXGPU_WARN("Temporal upscaler: {} upscales up to {}x; at the {:.2f}x scale the scene is drawn at the scale "
                  "and antialiased instead", name, upscaler_->MaxUpscaleRatio(), targets_->scale4() / 4.0);
  }
  auto camera = scene::DeriveCamera(slot.view);
  const char* skip = target.depth || target.samples != VK_SAMPLE_COUNT_1_BIT || target.fixed16 ? "target format/MSAA" :
                     !camera.valid ? "no perspective camera" : nullptr;
  bool created = false;
  // Scene drawn below the scale (native_scene_upscale): the output goes to the
  // surface's image at the scale, at that size.
  RenderTarget* output = target.scale4 != targets_->scale4() ? targets_->Instance(target, 0) : &target;
  // The view's part goes back; the upscaler takes the images' whole (largest
  // view's) size, so a letterbox step neither re-creates it nor loses history.
  const uint32_t output_width = output ? ScaleSize(slot.view.guest_width, output->scale4) : 0;
  const uint32_t output_height = output ? ScaleSize(slot.view.guest_height, output->scale4) : 0;
  const uint32_t render_width = ExtentWidth(slot.view), render_height = ExtentHeight(slot.view);
  const uint32_t extent_output_width =
      output ? std::max(ScaleSize(extent_guest_width_, output->scale4), output_width) : 0;
  const uint32_t extent_output_height =
      output ? std::max(ScaleSize(extent_guest_height_, output->scale4), output_height) : 0;
  if (!skip && !output) skip = "output target";
  if (!skip && !EnsureShared(upscaled_, VK_FORMAT_R16G16B16A16_SFLOAT, true, created, extent_output_width,
                             extent_output_height))
    skip = "output image";
  if (skip) { ++upscaler_skips_; upscaler_state_ = std::string("skipped: ") + skip; return false; }
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions();
  if (created) {
    VkDescriptorImageInfo info{linear_, upscaled_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{}; write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = upscaled_set_;
    write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo = &info;
    fn.vkUpdateDescriptorSets(device->device(), 1, &write, 0, nullptr);
  }
  auto* commands = submitter_->OpenOutsideRenderPass(); if (!commands) return false;
  slot.retire = std::max(slot.retire, submitter_->current());
  PadColor(commands, slot);
  for (ResolvedTexture* image : {&slot.images[kColor], &ndc_depth_, &slot.images[kMotion], &slot.images[kMasks]})
    TransitionTrackedImage(fn, commands, image->image, VK_IMAGE_ASPECT_COLOR_BIT, image->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionTrackedImage(fn, commands, upscaled_.image, VK_IMAGE_ASPECT_COLOR_BIT, upscaled_.layout, VK_IMAGE_LAYOUT_GENERAL);
  // The provider's depth is guest clip z/w: its inversion is the projection's.
  camera.depth_inverted = camera.ndc_inverted;
  TemporalUpscaler::Frame frame;
  frame.color = &slot.images[kColor]; frame.color_format = slot.view.color_format; frame.depth = &ndc_depth_; frame.motion = &slot.images[kMotion];
  // XeSS decides itself (native_xess_responsive_mask).
  const bool reactive = provider == UpscalerProvider::kXess || REXCVAR_GET(native_fsr_reactive);
  frame.reactive = reactive ? &slot.images[kMasks] : nullptr; frame.output = &upscaled_;
  frame.width = render_width; frame.height = render_height; frame.jitter = slot.view.jitter;
  frame.output_width = extent_output_width; frame.output_height = extent_output_height;
  frame.reset = !history; frame.camera = camera;
  frame.delta_ms = slot.view.delta_time * 1000.f; frame.meters_per_unit = slot.view.meters_per_unit;
  if (!upscaler_->Dispatch(frame)) {
    ++upscaler_skips_; upscaler_state_ = "skipped: " + upscaler_->last_error(); return false;
  }
  targets_->NoteGraphicsStateChange();  // The provider leaves its own bindings.
  // Back into the title's scene color before its composition: RGB only, the
  // title's destination alpha stays.
  // At the scale the output's surface image takes the EDRAM first (the 1x
  // image's content moves there, alpha included), then RGB is replaced.
  // At the viewport's origin (letterboxed scenes): the target's own, or the scale's.
  const Rect rect{int32_t(slot.view.guest_origin_x), int32_t(slot.view.guest_origin_y),
                  int32_t(slot.view.guest_origin_x + slot.view.guest_width),
                  int32_t(slot.view.guest_origin_y + slot.view.guest_height)};
  const uint32_t output_x = output == &target ? slot.view.origin_x : ScaleSize(slot.view.guest_origin_x, output->scale4);
  const uint32_t output_y = output == &target ? slot.view.origin_y : ScaleSize(slot.view.guest_origin_y, output->scale4);
  if (output != &target) targets_->Claim(*output, rect, 0, RenderTargets::ClaimCaller::kDraw);
  if (!DrawImage(*output, upscaled_, upscaled_set_, output_width, output_height, 7, output_x, output_y)) {
    ++failures_; upscaler_state_ = "failed: write-back";
    WriteUpscalerStatus(*submitter_, name + " failed (output)");
    return false;
  }
  if (output != &target) targets_->edram().Written(*output, rect, kEdramPartMain);
  ++upscaled_count_;
  const std::string version = upscaler_->version().empty() ? std::string("(version unknown)") : upscaler_->version();
  upscaler_state_ = "active: " + name + " " + version;
  WriteUpscalerStatus(*submitter_, name + " " + version + upscaler_->fallback_note() +
                                       (upscale_limited_ ? fmt::format(" (scale {:.2f}x above its {}x: native AA)",
                                                                       targets_->scale4() / 4.0,
                                                                       upscaler_->MaxUpscaleRatio())
                                                         : std::string()));
  return true;
}
bool SceneFrames::ReadImage(ResolvedTexture& image, std::vector<uint8_t>& bytes, uint32_t width, uint32_t height) {
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr;
  width = std::min(width, image.host_width); height = std::min(height, image.host_height);
  const VkDeviceSize size = VkDeviceSize(width) * height * BytesPerPixel(image.vk_format);
  bool result = false, recorded = false;
  do {
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        ui::vulkan::util::MemoryPurpose::kReadback, buffer, memory)) break;
    if (fn.vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) break;
    auto* commands = submitter_->OpenOutsideRenderPass(); if (!commands) break;
    recorded = true;
    TransitionTrackedImage(fn, commands, image.image, VK_IMAGE_ASPECT_COLOR_BIT, image.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    auto* copy = commands->CmdCopyImageToBufferEmplace(image.image, image.layout, buffer, 1);
    *copy = {}; copy->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; copy->imageSubresource.layerCount = 1;
    copy->imageExtent = {width, height, 1};
    VkMemoryBarrier barrier{}; barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    commands->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (!submitter_->Await(submitter_->current())) break;
    VkMappedMemoryRange range{}; range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE; range.memory = memory; range.size = VK_WHOLE_SIZE;
    if (fn.vkInvalidateMappedMemoryRanges(vk, 1, &range) != VK_SUCCESS) break;
    bytes.resize(size_t(size)); std::memcpy(bytes.data(), mapped, size_t(size)); result = true;
  } while (false);
  if (recorded) submitter_->AwaitAll();
  if (mapped) fn.vkUnmapMemory(vk, memory);
  if (buffer) fn.vkDestroyBuffer(vk, buffer, nullptr);
  if (memory) fn.vkFreeMemory(vk, memory, nullptr);
  return result;
}
bool SceneFrames::Export(const std::filesystem::path& base) {
  images_exported_ = false;
  if (!retained_ || current_ >= kNoSlot) return false;
  static constexpr const char* names[] = {".color.rgba16f", ".depth.r32f", ".camera-motion.rgba16f",
                                          ".reprojection.rgba16f", ".masks.rgba8"};
  bool result = true;
  for (size_t i = 0; i < kImageCount; ++i) {
    std::vector<uint8_t> bytes;
    const auto& view = slots_[current_].view;
    if (!ReadImage(slots_[current_].images[i], bytes, view.width, view.height)) { result = false; continue; }
    std::ofstream file(base.string() + names[i], std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); file.flush(); result = bool(file) && result;
  }
  images_exported_ = result;
  if (!result) ++failures_;
  return result;
}
void SceneFrames::Report(const std::filesystem::path& path) {
  std::ofstream file(path); file.imbue(std::locale::classic());
  file << std::setprecision(std::numeric_limits<float>::max_digits10);
  file << "{\"schema\":2,\"retained\":" << retained_count_ << ",\"dispatches\":" << dispatch_count_
       << ",\"debug_outputs\":" << debug_count_ << ",\"busy_skips\":" << busy_skips_ << ",\"rejected\":" << rejected_
       << ",\"failures\":" << failures_ << ",\"resets\":" << resets_ << ",\"title_resets\":" << title_resets_
       << ",\"jittered\":" << jittered_ << ",\"jitter_draws\":" << jitter_draws_ << ",\"opaque_snapshots\":" << opaque_count_
       << ",\"upscaler\":\"" << upscaler_state_ << "\",\"upscaled\":" << upscaled_count_
       << ",\"upscaler_skips\":" << upscaler_skips_
       << ",\"upscaled_width\":" << upscaled_.host_width << ",\"upscaled_height\":" << upscaled_.host_height
       << ",\"locked_volume_frames\":" << locked_frames_
       << ",\"frame_generation\":{\"written\":" << fg_written_ << ",\"hudless\":" << fg_hudless_count_
       << ",\"skips\":" << fg_skips_ << ",\"last_skip\":\"" << fg_skip_reason_ << "\",\"reset_reasons\":{";
  {
    bool first_reason = true;
    for (const auto& [reason, count] : reset_reasons_) {
      file << (first_reason ? "" : ",") << "\"" << reason << "\":" << count;
      first_reason = false;
    }
  }
  file << "},\"implicit_ends\":"
       << fg_implicit_ends_ << ",\"skip_reasons\":{";
  {
    bool first_reason = true;
    for (const auto& [reason, count] : fg_skip_reasons_) {
      file << (first_reason ? "" : ",") << "\"" << reason << "\":" << count;
      first_reason = false;
    }
  }
  file << "}}"
       << ",\"image_growths\":" << image_growths_ << ",\"retain_slow\":" << retain_slow_
       << ",\"retain_max_ms\":" << retain_max_ms_ << ",\"extent_guest_width\":" << extent_guest_width_
       << ",\"extent_guest_height\":" << extent_guest_height_ << ",\"extent_continues\":" << extent_continues_ << ",\"object_motion_dispatches\":" << object_frames_ << ",\"object_motion\":";
  if (object_motion_) object_motion_->Report(file); else file << "null";
  file
       << ",\"scene_interval\":" << (targets_ && targets_->scene_interval() ? "true" : "false") << ",\"reset_reason\":\"" << reset_reason_
       << "\",\"last_reject_reason\":\"" << last_reject_reason_ << "\",\"reject_reasons\":{";
  {
    bool first_reason = true;
    for (const auto& [reason, count] : reject_reasons_) {
      file << (first_reason ? "" : ",") << "\"" << reason << "\":" << count;
      first_reason = false;
    }
  }
  file << "},\"last_reject_frame\":" << last_reject_frame_
       << ",\"last_reject_session\":" << last_reject_session_ << ",\"previous_lease\":" << previous_lease_
       << ",\"completed_submission\":" << submitter_->completed()
       << ",\"current_submission\":" << submitter_->current() << ",\"current_retained\":" << (retained_ ? "true" : "false")
       << ",\"images_exported\":" << (images_exported_ ? "true" : "false")
       << ",\"output_width\":" << output_width_ << ",\"output_height\":" << output_height_ << ",\"output_mask\":" << output_mask_;
  if (current_ < kNoSlot) {
    const auto& slot = slots_[current_]; const auto& view = slot.view;
    file << ",\"lease\":" << slot.lease << ",\"session\":" << view.session << ",\"frame\":" << view.frame
         << ",\"width\":" << view.width << ",\"height\":" << view.height << ",\"source_color_id\":" << view.color_id
         << ",\"source_depth_id\":" << view.depth_id << ",\"retire_submission\":" << slot.retire
         << ",\"source_color_format\":" << view.color_format << ",\"source_depth_format\":" << view.depth_format
         << ",\"current_pv\":"; JsonArray(file, view.pv);
    file << ",\"ndc_scale\":"; JsonArray(file, view.ndc_scale);
    file << ",\"ndc_offset\":"; JsonArray(file, view.ndc_offset);
    file << ",\"depth_range\":"; JsonArray(file, view.depth_range);
    file << ",\"jitter\":"; JsonArray(file, view.jitter);
    const auto camera = scene::DeriveCamera(view);
    file << ",\"view_id\":" << view.view_id << ",\"delta_time\":" << view.delta_time
         << ",\"title_reset_reason\":" << view.title_reset_reason << ",\"locked_distance\":" << view.locked_distance
         << ",\"camera\":{\"valid\":" << (camera.valid ? "true" : "false")
         << ",\"near\":" << camera.near_distance << ",\"far\":" << (camera.infinite_far ? -1. : camera.far_distance)
         << ",\"fov_y\":" << camera.fov_y << ",\"aspect\":" << camera.aspect
         << ",\"depth_inverted\":" << (camera.depth_inverted ? "true" : "false")
         << ",\"far_host_depth\":" << camera.far_host_depth << ",\"locked_host_depth\":"
         << scene::LockedHostDepth(view, camera) << '}';
    if (previous_ < kNoSlot) {
      file << ",\"previous_frame\":" << slots_[previous_].view.frame << ",\"previous_pv\":";
      JsonArray(file, slots_[previous_].view.pv);
    }
  }
  file << ",\"slots\":[";
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (i) file << ',';
    const auto& slot = slots_[i]; file << "{\"lease\":" << slot.lease << ",\"retire\":" << slot.retire
      << ",\"width\":" << slot.images[0].host_width << ",\"height\":" << slot.images[0].host_height << '}';
  }
  file << "],\"motion_direction\":\"current-to-previous, scene pixels, without jitter, xy; expected previous host depth z; coverage w\""
          ",\"jitter_convention\":\"host pixels; scene content moved +x right, +y down\""
          ",\"masks\":\"RGBA8: reactive, camera-locked, sky/far, history coverage\""
          ",\"color_encoding\":\"RGBA16F; retains source range, not automatic HDR\",\"depth_encoding\":\"raw host R32F\""
          ",\"scope\":\"Camera motion from depth; camera-locked and sky classes; reactive from the opaque snapshot. No per-object motion.\"}\n";
  file.flush(); if (!file) { ++failures_; REXGPU_ERROR("Native scene frame report: {}", path.string()); }
}

void SceneFrames::NoteFrameMarker(const title::SceneMarker& marker) {
  // A frame's markers may arrive without its kFrameEnd (ring space): a new
  // frame starts over.
  if (fg_building_.session != marker.session || fg_building_.frame != marker.frame) {
    fg_building_ = {};
    fg_building_.session = marker.session;
    fg_building_.frame = marker.frame;
  }
  if (marker.flags & title::kSceneNoFrameGeneration) fg_building_.no_generation = true;
  if (marker.frame_width && marker.frame_height) {
    fg_building_.rect[0] = marker.frame_x; fg_building_.rect[1] = marker.frame_y;
    fg_building_.rect[2] = marker.frame_width; fg_building_.rect[3] = marker.frame_height;
  }
}

bool SceneFrames::SnapshotHudless(RenderTarget& color, uint32_t guest_width, uint32_t guest_height) {
  if (!submitter_ || !fg_building_.retained || fg_building_.hudless) return false;
  const uint32_t width = ScaleSize(guest_width, color.scale4), height = ScaleSize(guest_height, color.scale4);
  const char* skip = color.depth || color.samples != VK_SAMPLE_COUNT_1_BIT ? "hudless-target-format" :
      color.layout == VK_IMAGE_LAYOUT_UNDEFINED ? "hudless-undefined" :
      !width || !height || width > color.host_width || height > color.host_height ? "hudless-extent" : nullptr;
  if (skip) { ++fg_skips_; fg_skip_reason_ = skip; ++fg_skip_reasons_[skip]; return false; }
  if (hudless_.host_width != width || hudless_.host_height != height) {
    if (hudless_.image && !submitter_->AwaitAll()) return false;  // Earlier frames may still read it.
    DestroyImage(hudless_);
    if (!CreateImage(hudless_, width, height, VK_FORMAT_R16G16B16A16_SFLOAT, false)) {
      DestroyImage(hudless_); ++failures_; return false;
    }
  }
  const Rect rect{0, 0, int32_t(width), int32_t(height)};
  if (!resolve_->Draw(color, hudless_, rect, color.fixed16 ? 32.f : 1.f, (1u << uint32_t(color.samples)) - 1u)) {
    ++failures_; return false;
  }
  hudless_guest_width_ = guest_width; hudless_guest_height_ = guest_height;
  fg_building_.hudless = true; ++fg_hudless_count_;
  return true;
}

void SceneFrames::EndFrame(uint64_t session, uint64_t frame) {
  fg_ended_ = fg_building_.session == session && fg_building_.frame == frame ? fg_building_ : FrameGenerationFrame{};
  fg_building_ = {};
}

bool SceneFrames::EnsureFrameGenerationPipeline() {
  if (fg_pipeline_) return true;
  if (fg_pipeline_failed_ || !submitter_) return false;
  fg_pipeline_failed_ = true;  // Until everything below exists.
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  std::vector<uint32_t> spirv; std::string error;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, kFrameShader, spirv, error)) {
    REXGPU_ERROR("Native frame generation inputs shader: {}", error); return false;
  }
  fg_shader_ = ui::vulkan::util::CreateShaderModule(device, spirv.data(), spirv.size() * 4);
  if (!fg_shader_) return false;
  std::array<VkDescriptorSetLayoutBinding, 6> bindings{};
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = i; bindings[i].descriptorCount = 1; bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  }
  VkDescriptorSetLayoutCreateInfo set{}; set.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set.bindingCount = uint32_t(bindings.size()); set.pBindings = bindings.data();
  if (fn.vkCreateDescriptorSetLayout(vk, &set, nullptr, &fg_set_layout_) != VK_SUCCESS) return false;
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FrameConstants)};
  VkPipelineLayoutCreateInfo layout{}; layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layout.setLayoutCount = 1; layout.pSetLayouts = &fg_set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
  if (fn.vkCreatePipelineLayout(vk, &layout, nullptr, &fg_pipeline_layout_) != VK_SUCCESS) return false;
  VkComputePipelineCreateInfo pipe{}; pipe.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pipe.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  pipe.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; pipe.stage.module = fg_shader_; pipe.stage.pName = "main";
  pipe.layout = fg_pipeline_layout_;
  if (fn.vkCreateComputePipelines(vk, VK_NULL_HANDLE, 1, &pipe, nullptr, &fg_pipeline_) != VK_SUCCESS) {
    fg_pipeline_ = VK_NULL_HANDLE; return false;
  }
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(3 * kSlotCount)},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, uint32_t(3 * kSlotCount)}};
  VkDescriptorPoolCreateInfo pool{}; pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = uint32_t(kSlotCount); pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
  if (fn.vkCreateDescriptorPool(vk, &pool, nullptr, &fg_pool_) != VK_SUCCESS) return false;
  std::array<VkDescriptorSetLayout, kSlotCount> layouts; layouts.fill(fg_set_layout_);
  VkDescriptorSetAllocateInfo alloc{}; alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = fg_pool_; alloc.descriptorSetCount = uint32_t(kSlotCount); alloc.pSetLayouts = layouts.data();
  if (fn.vkAllocateDescriptorSets(vk, &alloc, fg_sets_.data()) != VK_SUCCESS) return false;
  fg_pipeline_failed_ = false;
  return true;
}

bool SceneFrames::WriteFrameGeneration(const ui::vulkan::VulkanPresenter::FrameGenerationTargets& targets,
                                       uint32_t output_width, uint32_t output_height, uint32_t guest_width,
                                       uint32_t guest_height, HudlessSource& hudless) {
  hudless = {};
  // The presented frame's kFrameEnd may come after its swap (a producer that
  // found the ring segment full places it then): the frame being built, its
  // scene and HUD-less image complete, is the one presented.
  if (!fg_ended_.retained && fg_building_.retained && fg_building_.hudless) {
    fg_ended_ = fg_building_;
    const uint64_t session = fg_building_.session, frame = fg_building_.frame;
    fg_building_ = {}; fg_building_.session = session; fg_building_.frame = frame;
    ++fg_implicit_ends_;
  }
  const auto& f = fg_ended_;
  const char* skip = !submitter_ ? "no-consumer" : !f.retained ? "no-view" : !f.hudless ? "no-hudless" :
      f.slot >= kNoSlot || slots_[f.slot].lease != f.lease || f.slot != current_ ? "slot-reused" :
      !output_width || !output_height || !guest_width || !guest_height ? "no-output" :
      hudless_.host_width != output_width || hudless_.host_height != output_height ||
      hudless_guest_width_ != guest_width || hudless_guest_height_ != guest_height ? "hudless-size" :
      !targets.depth || !targets.motion || !targets.depth_view || !targets.motion_view ? "no-targets" : nullptr;
  auto& slot = slots_[f.slot < kNoSlot ? f.slot : 0];
  if (!skip && (ndc_depth_.host_width < slot.view.width || ndc_depth_.host_height < slot.view.height))
    skip = "depth-size";
  const bool debug_view = targets.debug == 4;
  if (!skip && !EnsureFrameGenerationPipeline()) skip = "pipeline";
  bool created = false;
  if (!skip && debug_view &&
      !EnsureShared(fg_view_, VK_FORMAT_R16G16B16A16_SFLOAT, true, created, output_width, output_height))
    skip = "view-image";
  if (skip) { ++fg_skips_; fg_skip_reason_ = skip; ++fg_skip_reasons_[skip]; fg_ended_ = {}; return false; }
  // A descriptor set no recorded dispatch still reads.
  const size_t set_index = fg_next_set_; fg_next_set_ = (fg_next_set_ + 1) % fg_sets_.size();
  if (fg_set_retire_[set_index] && !submitter_->Await(fg_set_retire_[set_index])) { ++failures_; return false; }
  auto* commands = submitter_->OpenOutsideRenderPass(); if (!commands) return false;
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions();
  slot.retire = std::max(slot.retire, submitter_->current());
  fg_set_retire_[set_index] = submitter_->current();
  TransitionTrackedImage(fn, commands, slot.images[kMotion].image, VK_IMAGE_ASPECT_COLOR_BIT, slot.images[kMotion].layout,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionTrackedImage(fn, commands, ndc_depth_.image, VK_IMAGE_ASPECT_COLOR_BIT, ndc_depth_.layout,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // The presenter's images: whole images rewritten, earlier contents dropped.
  ImageBarrier(fn, commands, targets.depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  ImageBarrier(fn, commands, targets.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  if (debug_view)
    TransitionTrackedImage(fn, commands, fg_view_.image, VK_IMAGE_ASPECT_COLOR_BIT, fg_view_.layout, VK_IMAGE_LAYOUT_GENERAL);
  TransitionTrackedImage(fn, commands, hudless_.image, VK_IMAGE_ASPECT_COLOR_BIT, hudless_.layout,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  std::array<VkDescriptorImageInfo, 6> infos{};
  infos[0] = {nearest_, slot.images[kMotion].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[1] = {nearest_, ndc_depth_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[2] = {nearest_, hudless_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[3] = {VK_NULL_HANDLE, targets.depth_view, VK_IMAGE_LAYOUT_GENERAL};
  infos[4] = {VK_NULL_HANDLE, targets.motion_view, VK_IMAGE_LAYOUT_GENERAL};
  // Without the diagnostics view the shader doesn't write binding 5.
  infos[5] = {VK_NULL_HANDLE, debug_view ? fg_view_.view : targets.motion_view, VK_IMAGE_LAYOUT_GENERAL};
  std::array<VkWriteDescriptorSet, 6> writes{};
  for (uint32_t i = 0; i < writes.size(); ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = fg_sets_[set_index];
    writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[i].pImageInfo = &infos[i];
  }
  fn.vkUpdateDescriptorSets(device->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  // The scene in the frame, output pixels (the title's rectangle, else at the origin 1:1).
  const auto& view = slot.view;
  const double sx = double(output_width) / guest_width, sy = double(output_height) / guest_height;
  const bool has_rect = f.rect[2] && f.rect[3];
  const double rx = (has_rect ? f.rect[0] : view.guest_origin_x) * sx;
  const double ry = (has_rect ? f.rect[1] : view.guest_origin_y) * sy;
  const double rw = (has_rect ? f.rect[2] : view.guest_width) * sx, rh = (has_rect ? f.rect[3] : view.guest_height) * sy;
  const auto camera = scene::DeriveCamera(view);
  FrameConstants constants{};
  constants.rect = {float(rx), float(ry), float(rw), float(rh)};
  constants.scale = {float(rw / view.width), float(rh / view.height), camera.ndc_inverted ? 0.f : 1.f,
                     debug_view ? 1.f : 0.f};
  constants.size = {int32_t(view.width), int32_t(view.height), int32_t(output_width), int32_t(output_height)};
  fg_scale_ = {constants.scale[0], constants.scale[1]};
  // The frame this one's motion refers to: the scene retained before it (a
  // frame without a scene in between, e.g. a cutscene drawing every other
  // frame, keeps the history). 0: none.
  const bool motion_history = !f.reset && previous_ < kNoSlot && slots_[previous_].view.session == view.session;
  fg_motion_previous_frame_ = motion_history ? slots_[previous_].view.frame : 0;
  // The camera of the written frame before, when the motion refers to it
  // (DLSS frame generation's matrices).
  fg_has_previous_pv_ = fg_motion_previous_frame_ && fg_pv_session_ == view.session &&
                        fg_pv_frame_ == fg_motion_previous_frame_;
  fg_previous_pv_ = fg_pv_;
  fg_pv_ = view.pv; fg_pv_session_ = view.session; fg_pv_frame_ = view.frame;
  // The motion's reference frame's rectangle when it was the frame written
  // before, else this one's.
  constants.previous_rect = fg_has_previous_pv_ ? fg_rect_ : constants.rect;
  {
    const auto& reference = motion_history ? slots_[previous_].view : view;
    constants.previous_size = {float(reference.width), float(reference.height), 0.f, 0.f};
  }
  fg_rect_ = constants.rect;
  // The scene's clip space in the frame's (the depth and motion images cover
  // the frame; a letterboxed scene is its part): x' = sx x + ox w, y' = sy y +
  // oy w, guest NDC y up, frame pixels y down.
  fg_previous_frame_clip_ = fg_frame_clip_;
  fg_frame_clip_ = {rw / output_width, (2. * rx + rw) / output_width - 1., rh / output_height,
                    1. - (2. * ry + rh) / output_height};
  commands->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, fg_pipeline_);
  commands->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, fg_pipeline_layout_, 0, 1, &fg_sets_[set_index], 0, nullptr);
  commands->CmdVkPushConstants(fg_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
  commands->CmdVkDispatch((output_width + 7) / 8, (output_height + 7) / 8, 1);
  // Released like the guest output image.
  ImageBarrier(fn, commands, targets.depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
               ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
  ImageBarrier(fn, commands, targets.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
               ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
  ResolvedTexture& source = debug_view ? fg_view_ : hudless_;
  if (debug_view)
    TransitionTrackedImage(fn, commands, fg_view_.image, VK_IMAGE_ASPECT_COLOR_BIT, fg_view_.layout,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  hudless.view = source.view; hudless.width = output_width; hudless.height = output_height;
  fg_written_frame_ = f; fg_ended_ = {};
  ++fg_written_;
  return true;
}

void SceneFrames::FillFrameGenerationInputs(ui::Presenter::FrameGenerationInputs& inputs) const {
  const auto& f = fg_written_frame_;
  if (f.slot >= kNoSlot) return;
  const auto& view = slots_[f.slot].view;
  const auto camera = scene::DeriveCamera(view);
  inputs.frame = f.frame;
  inputs.generate = !f.no_generation && camera.valid;
  inputs.reset = f.reset;
  inputs.delta_ms = view.delta_time * 1000.f;
  inputs.jitter_x = view.jitter[0] * fg_scale_[0];
  inputs.jitter_y = view.jitter[1] * fg_scale_[1];
  inputs.camera_near = float(camera.near_distance);
  inputs.camera_far = float(camera.infinite_far ? 0. : camera.far_distance);
  inputs.fov_y = float(camera.fov_y);
  inputs.meters_per_unit = view.meters_per_unit;
  // The inputs' depth is guest clip z/w: its inversion is the projection's.
  inputs.depth_inverted = camera.ndc_inverted;
  inputs.depth_infinite = camera.infinite_far;
  CameraAxes(view, inputs.position, inputs.up, inputs.right, inputs.forward);
  // Matrices in the clip space of the depth and motion images (the whole
  // frame: a letterboxed scene's clip space mapped into it, the previous frame
  // with its own rectangle), z/w as the guest's, without jitter; the
  // reprojection in float64 (a far origin loses precision in float32).
  const scene::Matrix identity = scene::Identity();
  auto frame_clip = [](const std::array<double, 4>& n) {
    scene::DoubleMatrix t{};
    t[0] = n[0]; t[3] = n[1]; t[5] = n[2]; t[7] = n[3]; t[10] = 1.; t[15] = 1.;
    return t;
  };
  auto to_double = [](const scene::Matrix& m) {
    scene::DoubleMatrix d{};
    for (size_t i = 0; i < 16; ++i) d[i] = m[i];
    return d;
  };
  auto to_float = [](const scene::DoubleMatrix& d) {
    scene::Matrix m{};
    for (size_t i = 0; i < 16; ++i) m[i] = float(d[i]);
    return m;
  };
  const scene::DoubleMatrix frame = frame_clip(fg_frame_clip_);
  inputs.view_to_clip = to_float(scene::MultiplyDouble(frame, to_double(view.projection)));
  if (!scene::Invert(inputs.view_to_clip, inputs.clip_to_view)) inputs.clip_to_view = identity;
  inputs.clip_to_previous_clip = inputs.previous_clip_to_clip = identity;
  scene::DoubleMatrix inverse_pv, inverse_back;
  if (fg_has_previous_pv_ && !f.reset &&
      scene::InvertDouble(to_float(scene::MultiplyDouble(frame, to_double(fg_pv_))), inverse_pv)) {
    const scene::DoubleMatrix previous =
        scene::MultiplyDouble(frame_clip(fg_previous_frame_clip_), to_double(fg_previous_pv_));
    const scene::DoubleMatrix to_previous = scene::MultiplyDouble(previous, inverse_pv);
    scene::Matrix to_previous_float{};
    bool finite = true;
    for (size_t i = 0; i < 16; ++i) {
      to_previous_float[i] = float(to_previous[i]);
      finite = finite && std::isfinite(to_previous_float[i]);
    }
    if (finite && scene::InvertDouble(to_previous_float, inverse_back)) {
      inputs.clip_to_previous_clip = to_previous_float;
      for (size_t i = 0; i < 16; ++i) inputs.previous_clip_to_clip[i] = float(inverse_back[i]);
    }
  }
  // The frame's: as the matrices (a letterboxed scene's is wider than the frame's).
  inputs.aspect = fg_frame_clip_[2] > 0 && fg_frame_clip_[0] > 0
                      ? float(camera.aspect * fg_frame_clip_[2] / fg_frame_clip_[0]) : float(camera.aspect);
  inputs.previous_frame = fg_motion_previous_frame_;
}
}  // namespace rex::graphics::native
