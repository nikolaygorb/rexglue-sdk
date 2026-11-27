#include <rex/graphics/native/object_motion.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>

#include <rex/graphics/native/image_barrier.h>
#include <rex/graphics/native/scene_frames.h>
#include <rex/graphics/vulkan/glsl_compile.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DEFINE_BOOL(native_scene_object_motion, true, "GPU",
    "Motion of moving and animated objects for the temporal upscaler and frame generation: the scene "
    "draws' vertex positions are kept and drawn again with the previous frame's (camera motion "
    "alone without). Needs native_temporal_upscaler or native_frame_generation at start.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_scene_object_motion_vertices, 2 << 20, "GPU",
    "Object motion: vertices of the scene draws kept per frame (20 bytes each, two frames). Draws "
    "beyond it keep camera motion.")
    .range(1 << 16, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(native_scene_object_motion_identity, "", "GPU",
    "Object motion: the vertex shader float constant registers that tell copies of one mesh apart, "
    "\"first-last\" (e.g. the object's world transform, \"80-83\"). A draw whose shader reads all of "
    "them is matched to the previous frame's copy by them only; empty, \"off\" or a shader without "
    "them: by its first used constants (camera-dependent ones too, so copies can swap while the "
    "camera turns).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::native {
namespace {
// Host clip positions of the draw (current) and of the same draw in the
// previous frame, from the capture buffer by the host vertex index.
constexpr char kVertexShader[] = R"glsl(#version 460
layout(std430, binding=0) readonly buffer Capture { uint words[]; } capture;
layout(push_constant) uniform Constants {
  uvec4 current;   // base, mask, tag, index endian (xenos::Endian)
  uvec4 previous;  // base, mask, tag, present
  vec4 jitter;     // current NDC xy, previous NDC zw
  vec4 depth;      // previous min, max; current min, max
  vec4 sample_offset;
} c;
layout(location=0) out vec4 current_clip;
layout(location=1) out vec4 previous_clip;
layout(location=2) out float valid;
vec4 Load(uvec4 region, uint index, out bool ok) {
  uint entry = (region.x + (index & region.y)) * 5u;
  ok = capture.words[entry + 4u] == ((index & 0xFFFFFFu) | region.z);
  return uintBitsToFloat(uvec4(capture.words[entry], capture.words[entry + 1u],
                               capture.words[entry + 2u], capture.words[entry + 3u]));
}
// The guest index as the draw's vertex shader stored it (Vulkan reads guest
// indices in their byte order).
uint Swap(uint v, uint endian) {
  if (endian == 1u || endian == 2u) v = ((v & 0x00FF00FFu) << 8u) | ((v >> 8u) & 0x00FF00FFu);
  if (endian == 2u || endian == 3u) v = (v << 16u) | (v >> 16u);
  return v;
}
void main() {
  uint index = Swap(uint(gl_VertexIndex), c.current.w);
  bool ok;
  vec4 position = Load(c.current, index, ok);
  // Not this draw's vertex (another index in the slot): the primitive is culled.
  if (!ok) position = vec4(uintBitsToFloat(0x7FC00000u));
  bool previous_ok = false;
  vec4 previous = vec4(0.0, 0.0, 0.0, 1.0);
  if (c.previous.w != 0u) previous = Load(c.previous, index, previous_ok);
  gl_Position = position;
  current_clip = position;
  previous_clip = previous;
  valid = previous_ok && previous.w > 0.0 ? 1.0 : 0.0;
}
)glsl";
// Where the draw is the visible surface (its depth at the retained depth's
// sample): the previous uv (without jitter) and depth of that surface point.
constexpr char kFragmentShader[] = R"glsl(#version 460
layout(binding=1) uniform sampler2D scene_depth;
layout(push_constant) uniform Constants {
  uvec4 current;
  uvec4 previous;
  vec4 jitter;
  vec4 depth;
  vec4 sample_offset;
} c;
layout(location=0) in vec4 current_clip;
layout(location=1) in vec4 previous_clip;
layout(location=2) in float valid;
layout(location=0) out vec4 result;
void main() {
  if (valid < 0.999) discard;
#ifdef AT_SAMPLE_OFFSET
  vec4 at_sample = interpolateAtOffset(current_clip, c.sample_offset.xy);
#else
  vec4 at_sample = current_clip;  // No interpolation functions (sampleRateShading).
#endif
  if (!(abs(at_sample.w) > 1e-20) || !(previous_clip.w > 1e-20)) discard;
  float depth = c.depth.z + clamp(at_sample.z / at_sample.w, 0.0, 1.0) * (c.depth.w - c.depth.z);
  float stored = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
  if (!(abs(depth - stored) <= 1e-6 + 1e-4 * abs(stored))) discard;
  vec2 ndc = previous_clip.xy / previous_clip.w - c.jitter.zw;
  float previous_depth =
      c.depth.x + clamp(previous_clip.z / previous_clip.w, 0.0, 1.0) * (c.depth.y - c.depth.x);
  result = vec4(ndc * 0.5 + 0.5, previous_depth, 1.0);
}
)glsl";
struct PassConstants {
  uint32_t current[4];
  uint32_t previous[4];
  float jitter[4];
  float depth[4];
  float sample_offset[4];
};
static_assert(sizeof(PassConstants) == 80);
uint32_t RegionSize(uint32_t vertex_count) {
  return std::bit_ceil(std::max(vertex_count, uint32_t(64)));
}
}  // namespace

ObjectMotion::~ObjectMotion() { Shutdown(); }

bool ObjectMotion::Requested() { return REXCVAR_GET(native_scene_object_motion) && SceneFramesEnabled(); }

bool ObjectMotion::Initialize(Submitter& submitter) {
  submitter_ = &submitter;
  const auto* device = submitter.vulkan_device();
  capacity_ = uint32_t(std::clamp(REXCVAR_GET(native_scene_object_motion_vertices), 1 << 16, 1 << 24));
  identity_first_ = identity_count_ = 0;
  if (const std::string& identity = REXCVAR_GET(native_scene_object_motion_identity); !identity.empty()) {
    char *rest = nullptr, *end = nullptr;
    const unsigned long first = std::strtoul(identity.c_str(), &rest, 10);
    const unsigned long last = *rest == '-' ? std::strtoul(rest + 1, &end, 10) : first;
    if (identity == "off") {
      REXGPU_INFO("Native object motion: copies of a mesh told apart by their first constants");
    } else if (rest != identity.c_str() && (*rest == '-' ? end != rest + 1 && !*end : !*rest) && first <= last &&
               last < 256 && last - first < kFingerprintRegisters) {
      identity_first_ = uint32_t(first); identity_count_ = uint32_t(last - first + 1);
      REXGPU_INFO("Native object motion: copies of a mesh told apart by c{}-c{}", first, last);
    } else {
      REXGPU_WARN("Native object motion: native_scene_object_motion_identity \"{}\" is not \"first-last\" "
                  "(at most {} registers below 256), ignored", identity, kFingerprintRegisters);
    }
  }
  const VkDeviceSize size = VkDeviceSize(capacity_) * 2 * kEntryDwords * sizeof(uint32_t);
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          device, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, buffer_, memory_)) {
    REXGPU_ERROR("Native object motion: no capture buffer of {} MB", size >> 20);
    buffer_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE;
    return false;
  }
  REXGPU_INFO("Native object motion: {} vertices per frame ({} MB)", capacity_, size >> 20);
  return true;
}

void ObjectMotion::DestroyOutput() {
  if (!submitter_) return;
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  if (output_.framebuffer) fn.vkDestroyFramebuffer(vk, output_.framebuffer, nullptr);
  if (output_.view) fn.vkDestroyImageView(vk, output_.view, nullptr);
  if (output_.image) fn.vkDestroyImage(vk, output_.image, nullptr);
  if (output_.memory) fn.vkFreeMemory(vk, output_.memory, nullptr);
  output_ = {};
}

void ObjectMotion::Shutdown() {
  if (!submitter_) return;
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  DestroyOutput();
  for (const auto& item : pipelines_) fn.vkDestroyPipeline(vk, item.second, nullptr);
  pipelines_.clear();
  if (pool_) fn.vkDestroyDescriptorPool(vk, pool_, nullptr);
  if (render_pass_) fn.vkDestroyRenderPass(vk, render_pass_, nullptr);
  if (pipeline_layout_) fn.vkDestroyPipelineLayout(vk, pipeline_layout_, nullptr);
  if (set_layout_) fn.vkDestroyDescriptorSetLayout(vk, set_layout_, nullptr);
  if (sampler_) fn.vkDestroySampler(vk, sampler_, nullptr);
  if (vertex_shader_) fn.vkDestroyShaderModule(vk, vertex_shader_, nullptr);
  if (fragment_shader_) fn.vkDestroyShaderModule(vk, fragment_shader_, nullptr);
  if (buffer_) fn.vkDestroyBuffer(vk, buffer_, nullptr);
  if (memory_) fn.vkFreeMemory(vk, memory_, nullptr);
  pool_ = VK_NULL_HANDLE; render_pass_ = VK_NULL_HANDLE; pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE; sampler_ = VK_NULL_HANDLE; vertex_shader_ = fragment_shader_ = VK_NULL_HANDLE;
  buffer_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE; sets_ = {}; set_retire_ = {};
  current_ = {}; previous_ = {}; previous_used_.clear(); armed_ = cleared_ = pipelines_failed_ = false;
  submitter_ = nullptr;
}

void ObjectMotion::Arm(uint64_t depth_id, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  if (!buffer_) return;
  if (!cleared_) {
    // No stale tags: the buffer starts zeroed (no tag is 0).
    auto* commands = submitter_->OpenOutsideRenderPass();
    if (!commands) return;
    commands->CmdVkFillBuffer(buffer_, 0, VK_WHOLE_SIZE, 0);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    commands->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1,
                                   &barrier, 0, nullptr, 0, nullptr);
    cleared_ = true;
  }
  armed_ = true; depth_id_ = depth_id; x_ = x; y_ = y; width_ = width; height_ = height;
}

bool ObjectMotion::Capture(Draw draw, const float* fingerprint, uint32_t fingerprint_count, uint32_t& base,
                           uint32_t& mask, uint32_t& tag) {
  if (!armed_ || !buffer_ || !draw.vertex_count) return false;
  const uint32_t region = RegionSize(draw.vertex_count);
  if (region > capacity_ - cursor_ || current_.draws.size() >= (1u << 16)) { ++budget_skips_; return false; }
  draw.base = half_ * capacity_ + cursor_; draw.mask = region - 1; draw.tag = salt_ << 24;
  cursor_ += region;
  fingerprint_count = std::min(fingerprint_count, kFingerprintRegisters * 4);
  draw.fingerprint_offset = uint32_t(current_.fingerprints.size()); draw.fingerprint_count = fingerprint_count;
  current_.fingerprints.insert(current_.fingerprints.end(), fingerprint, fingerprint + fingerprint_count);
  // The same draw in the previous frame: by key, the closest constants among
  // those not taken yet (the same mesh drawn several times).
  draw.previous = -1;
  float best = std::numeric_limits<float>::infinity();
  const auto range = previous_.by_key.equal_range(draw.key);
  for (auto it = range.first; it != range.second; ++it) {
    const uint32_t index = it->second;
    if (previous_used_[index]) continue;
    const Draw& candidate = previous_.draws[index];
    float distance = 0;
    if (candidate.fingerprint_count == fingerprint_count) {
      const float* other = previous_.fingerprints.data() + candidate.fingerprint_offset;
      for (uint32_t i = 0; i < fingerprint_count; ++i) {
        const float d = std::abs(fingerprint[i] - other[i]);
        distance += std::isfinite(d) ? d : 1e30f;
      }
    } else {
      distance = 1e30f;
    }
    if (distance < best || draw.previous < 0) { best = distance; draw.previous = int32_t(index); }
  }
  if (draw.previous >= 0) { previous_used_[size_t(draw.previous)] = true; ++matched_draws_; } else ++unmatched_draws_;
  current_.by_key.emplace(draw.key, uint32_t(current_.draws.size()));
  current_.draws.push_back(draw);
  ++captured_draws_; captured_vertices_ += region;
  base = draw.base; mask = draw.mask; tag = draw.tag;
  return true;
}

void ObjectMotion::Flip() {
  last_draws_ = uint32_t(current_.draws.size()); last_vertices_ = cursor_;
  peak_vertices_ = std::max(peak_vertices_, cursor_);
  previous_ = std::move(current_);
  current_ = {};
  previous_used_.assign(previous_.draws.size(), false);
  half_ ^= 1; cursor_ = 0;
  salt_ = salt_ >= 255 ? 1 : salt_ + 1;
}

void ObjectMotion::DropFrame() {
  if (current_.draws.empty() && !cursor_) return;
  // The previous frame stays the last retained one: its draws may match again.
  current_ = {}; cursor_ = 0;
  previous_used_.assign(previous_.draws.size(), false);
  salt_ = salt_ >= 255 ? 1 : salt_ + 1;
  ++dropped_frames_;
}

bool ObjectMotion::EnsurePipelines() {
  if (pipelines_failed_) return false;
  if (render_pass_) return true;
  pipelines_failed_ = true;  // Until everything below exists.
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  std::string error;
  // Depth at the retained sample's offset where interpolation functions exist.
  std::string fragment_source = kFragmentShader;
  if (device->properties().sampleRateShading) {
    fragment_source.insert(fragment_source.find('\n') + 1, "#define AT_SAMPLE_OFFSET 1\n");
  }
  std::vector<uint32_t> vertex_spirv, fragment_spirv;
  if (!vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_VERTEX_BIT, kVertexShader, vertex_spirv, error) ||
      !vulkan::CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, fragment_source, fragment_spirv, error)) {
    REXGPU_ERROR("Native object motion shaders: {}", error);
    return false;
  }
  vertex_shader_ = ui::vulkan::util::CreateShaderModule(device, vertex_spirv.data(), vertex_spirv.size() * 4);
  fragment_shader_ = ui::vulkan::util::CreateShaderModule(device, fragment_spirv.data(), fragment_spirv.size() * 4);
  if (!vertex_shader_ || !fragment_shader_) return false;
  std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
  bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = uint32_t(bindings.size()); set_info.pBindings = bindings.data();
  if (fn.vkCreateDescriptorSetLayout(vk, &set_info, nullptr, &set_layout_) != VK_SUCCESS) return false;
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PassConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1; layout_info.pPushConstantRanges = &push;
  if (fn.vkCreatePipelineLayout(vk, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;
  const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, uint32_t(kSetCount)},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(kSetCount)}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = uint32_t(kSetCount); pool_info.poolSizeCount = 2; pool_info.pPoolSizes = sizes;
  if (fn.vkCreateDescriptorPool(vk, &pool_info, nullptr, &pool_) != VK_SUCCESS) return false;
  for (auto& set : sets_) {
    VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = pool_; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &set_layout_;
    if (fn.vkAllocateDescriptorSets(vk, &alloc, &set) != VK_SUCCESS) return false;
  }
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (fn.vkCreateSampler(vk, &sampler, nullptr, &sampler_) != VK_SUCCESS) return false;
  VkAttachmentDescription attachment{};
  attachment.format = VK_FORMAT_R32G32B32A32_SFLOAT; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{}; subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1; subpass.pColorAttachments = &color;
  VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass_info.attachmentCount = 1; pass_info.pAttachments = &attachment; pass_info.subpassCount = 1;
  pass_info.pSubpasses = &subpass;
  if (fn.vkCreateRenderPass(vk, &pass_info, nullptr, &render_pass_) != VK_SUCCESS) return false;
  pipelines_failed_ = false;
  return true;
}

VkPipeline ObjectMotion::GetPipeline(VkPrimitiveTopology topology, bool primitive_restart) {
  const uint32_t key = uint32_t(topology) << 1 | uint32_t(primitive_restart);
  if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
  const auto* device = submitter_->vulkan_device();
  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  for (auto& stage : stages) { stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName = "main"; }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex_shader_;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment_shader_;
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = topology; assembly.primitiveRestartEnable = primitive_restart;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  // Depth as the draw had it, also beyond the clip planes (compared below).
  raster.depthClampEnable = device->properties().depthClamp;
  raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_CLOCKWISE; raster.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo msaa{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blending.attachmentCount = 1; blending.pAttachments = &blend;
  const VkDynamicState dynamic_states[]{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = 2; info.pStages = stages.data(); info.pVertexInputState = &vertex; info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport; info.pRasterizationState = &raster; info.pMultisampleState = &msaa;
  info.pColorBlendState = &blending; info.pDynamicState = &dynamic; info.layout = pipeline_layout_;
  info.renderPass = render_pass_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (device->functions().vkCreateGraphicsPipelines(device->device(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Native object motion: no pipeline for topology {}", uint32_t(topology));
    pipeline = VK_NULL_HANDLE;
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

bool ObjectMotion::EnsureOutput(uint32_t width, uint32_t height) {
  // Grows only: smaller views (letterbox steps) draw into its corner.
  if (output_.image && output_.host_width >= width && output_.host_height >= height) return true;
  if (output_.image) {
    width = std::max(width, output_.host_width); height = std::max(height, output_.host_height);
    if (!submitter_->Await(output_retire_)) return false;
  }
  DestroyOutput();
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions(); const auto vk = device->device();
  output_.vk_format = VK_FORMAT_R32G32B32A32_SFLOAT; output_.host_width = width; output_.host_height = height;
  output_.attachment = true;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D;
  info.format = output_.vk_format; info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(device, info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
                                                       output_.image, output_.memory)) {
    output_ = {}; return false;
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = output_.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = output_.vk_format;
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
  if (fn.vkCreateImageView(vk, &view, nullptr, &output_.view) != VK_SUCCESS) { DestroyOutput(); return false; }
  VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer.renderPass = render_pass_; framebuffer.attachmentCount = 1; framebuffer.pAttachments = &output_.view;
  framebuffer.width = width; framebuffer.height = height; framebuffer.layers = 1;
  if (fn.vkCreateFramebuffer(vk, &framebuffer, nullptr, &output_.framebuffer) != VK_SUCCESS) { DestroyOutput(); return false; }
  return true;
}

bool ObjectMotion::Record(ResolvedTexture& depth, uint32_t width, uint32_t height, VkSampleCountFlagBits depth_samples,
                          bool history) {
  ++frames_;
  bool any_history = false;
  if (!history) { Flip(); return false; }
  for (const Draw& draw : current_.draws) any_history |= draw.previous >= 0;
  if (!any_history || !width || !height || !EnsurePipelines() || !EnsureOutput(width, height)) {
    Flip();
    return false;
  }
  const auto* device = submitter_->vulkan_device(); const auto& fn = device->functions();
  // A descriptor set no submission still reads.
  const uint64_t completed = submitter_->completed();
  size_t set_index = kSetCount;
  for (size_t i = 0; i < kSetCount; ++i) {
    const size_t candidate = (next_set_ + i) % kSetCount;
    if (set_retire_[candidate] <= completed) { set_index = candidate; break; }
  }
  if (set_index == kSetCount) {
    set_index = next_set_;
    if (!submitter_->Await(set_retire_[set_index])) { Flip(); return false; }
  }
  next_set_ = (set_index + 1) % kSetCount;
  const VkDescriptorSet set = sets_[set_index];
  auto* commands = submitter_->OpenOutsideRenderPass();
  if (!commands) { Flip(); return false; }
  set_retire_[set_index] = submitter_->current(); output_retire_ = submitter_->current();
  VkDescriptorBufferInfo buffer_info{buffer_, 0, VK_WHOLE_SIZE};
  VkDescriptorImageInfo image_info{sampler_, depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  std::array<VkWriteDescriptorSet, 2> writes{};
  for (auto& write : writes) {
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = set; write.descriptorCount = 1;
  }
  writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &buffer_info;
  writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &image_info;
  fn.vkUpdateDescriptorSets(device->device(), uint32_t(writes.size()), writes.data(), 0, nullptr);
  // The scene draws' captures, before this pass reads them.
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  commands->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1,
                                 &barrier, 0, nullptr, 0, nullptr);
  TransitionTrackedImage(fn, commands, depth.image, VK_IMAGE_ASPECT_COLOR_BIT, depth.layout,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  TransitionTrackedImage(fn, commands, output_.image, VK_IMAGE_ASPECT_COLOR_BIT, output_.layout,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkClearValue clear{};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = render_pass_; begin.framebuffer = output_.framebuffer;
  begin.renderArea = {{0, 0}, {width, height}}; begin.clearValueCount = 1; begin.pClearValues = &clear;
  commands->CmdVkBeginRenderPass(&begin, VK_SUBPASS_CONTENTS_INLINE);
  // The scene's viewport moved to the image's origin: the same rasterization.
  VkViewport viewport{0.f, 0.f, float(width), float(height), 0.f, 1.f};
  VkRect2D scissor{{0, 0}, {width, height}};
  commands->CmdVkSetViewport(0, 1, &viewport); commands->CmdVkSetScissor(0, 1, &scissor);
  commands->CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
  // The retained depth is the target's sample 0 (standard sample locations).
  std::array<float, 2> sample_offset{};
  if (depth_samples == VK_SAMPLE_COUNT_2_BIT) sample_offset = {.25f, .25f};
  else if (depth_samples == VK_SAMPLE_COUNT_4_BIT) sample_offset = {-.125f, -.375f};
  VkPipeline bound = VK_NULL_HANDLE;
  for (const Draw& draw : current_.draws) {
    if (draw.previous < 0) continue;
    const Draw& previous = previous_.draws[size_t(draw.previous)];
    const VkPipeline pipeline = GetPipeline(draw.topology, draw.primitive_restart);
    if (!pipeline) continue;
    if (pipeline != bound) { commands->CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline); bound = pipeline; }
    PassConstants constants{};
    constants.current[0] = draw.base; constants.current[1] = draw.mask; constants.current[2] = draw.tag;
    constants.current[3] = draw.index_endian;
    constants.previous[0] = previous.base; constants.previous[1] = previous.mask; constants.previous[2] = previous.tag;
    constants.previous[3] = 1;
    constants.jitter[0] = draw.jitter[0]; constants.jitter[1] = draw.jitter[1];
    constants.jitter[2] = previous.jitter[0]; constants.jitter[3] = previous.jitter[1];
    constants.depth[0] = previous.depth_range[0]; constants.depth[1] = previous.depth_range[1];
    constants.depth[2] = draw.depth_range[0]; constants.depth[3] = draw.depth_range[1];
    constants.sample_offset[0] = sample_offset[0]; constants.sample_offset[1] = sample_offset[1];
    commands->CmdVkPushConstants(pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(constants), &constants);
    if (draw.index_buffer) {
      commands->CmdVkBindIndexBuffer(draw.index_buffer, draw.index_offset, draw.index_type);
      commands->CmdVkDrawIndexed(draw.vertex_count, 1, 0, 0, 0);
    } else {
      commands->CmdVkDraw(draw.vertex_count, 1, 0, 0);
    }
    ++recorded_draws_;
  }
  commands->CmdVkEndRenderPass();
  // The next frame's scene draws write where this pass read.
  commands->CmdVkPipelineBarrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 0, nullptr);
  Flip();
  return true;
}

void ObjectMotion::Report(std::ostream& file) const {
  file << "{\"enabled\":" << (buffer_ ? "true" : "false") << ",\"capacity_vertices\":" << capacity_
       << ",\"frames\":" << frames_ << ",\"captured_draws\":" << captured_draws_
       << ",\"captured_vertices\":" << captured_vertices_ << ",\"matched_draws\":" << matched_draws_
       << ",\"unmatched_draws\":" << unmatched_draws_ << ",\"recorded_draws\":" << recorded_draws_
       << ",\"budget_skips\":" << budget_skips_ << ",\"dropped_frames\":" << dropped_frames_
       << ",\"last_draws\":" << last_draws_ << ",\"last_vertices\":" << last_vertices_
       << ",\"peak_vertices\":" << peak_vertices_ << '}';
}

}  // namespace rex::graphics::native
