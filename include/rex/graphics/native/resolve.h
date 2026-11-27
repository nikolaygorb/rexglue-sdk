/**
 * @file        graphics/native/resolve.h
 * @brief       Native renderer: resolves into host textures, and their clears
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              Derived from ArmyOfTwoRecompilation (aot/src/gpu/native/renderer.cpp,
 *              RendererResolve, HostTexture), Copyright (c) 2026 OpenPortroid, MIT License
 *              (src/graphics/native/LICENSE.ArmyOfTwoRecompilation).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/graphics/native/command_recorder.h>
#include <rex/graphics/native/resolve_memory.h>
#include <rex/graphics/native/resolve_pass.h>
#include <rex/graphics/native/scale.h>
#include <rex/graphics/xenos.h>
#include <rex/ui/vulkan/api.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::graphics {
class RegisterFile;
}  // namespace rex::graphics

namespace rex::graphics::native {

class NativeSharedMemory;
class NativeTextureCache;
class RenderTargets;
class Submitter;

// The destination of resolves to one guest address (render to texture),
// holding the render target's values in its own channel order (red / blue as
// rendered; red_blue_swapped records what the guest's copy would have); a
// depth resolve's is R32_SFLOAT with the depth values (from_depth).
// The shared memory gets the guest's bytes of each resolve too (ResolveMemory:
// what textures of other formats and the CPU read); the range is marked as
// written by the GPU there: a CPU write to it drops the texture, and the
// guest's data is sampled from memory again.
struct ResolvedTexture {
  uint32_t address = 0;
  xenos::ColorFormat guest_format = xenos::ColorFormat::k_8_8_8_8;
  VkFormat vk_format = VK_FORMAT_UNDEFINED;
  // Guest texels; the host image is ScaleSize of them at scale4.
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t scale4 = kScale4Min;
  uint32_t host_width = 0;
  uint32_t host_height = 0;
  bool red_blue_swapped = false;
  bool from_depth = false;
  // The last resolve into it: its source's render target format (colour or
  // depth, by from_depth), whether that was a 16-bit fixed surface (the host
  // holds value / 32), and the copy's number format. For the log of a format
  // not composed yet (16-bit fixed textures: what the image and the texture
  // cache each hold depends on these).
  uint32_t source_format = 0;
  bool source_fixed16 = false;
  uint32_t dest_number = 0;
  // From D24FS8: the image holds the guest's [0, 2) (k_24_8_FLOAT), else
  // [0, 1] (k_24_8).
  bool depth_float24 = false;
  uint64_t last_frame = 0;
  // The guest memory the resolves to it would have written, [start, end).
  uint32_t written_start = 0;
  uint32_t written_end = 0;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  // For the translated shaders, which sample 2D textures as 2D arrays: with
  // the identity swizzle, and by other swizzles (Resolver::GetSampledView).
  VkImageView array_view = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkImageView> swizzled_views;
  VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  // Created with VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT: the resolve pass can
  // draw into it (through its framebuffer, created on first use).
  bool attachment = false;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;

  // Where the image's pixels are in guest memory, for sampling at the
  // resolution scale (milestone N6b). The image is in the copies'
  // destination coordinates (the source rectangle's own, as the GPU writes
  // them): pixel (x, y) is at dest_texture_base + the 2D tiled address of
  // (x + dest_x_offset, y) with dest_pitch_aligned (GetResolveDestOrigin).
  // placeable: all of that is known (2D, not an array; depth as k_24_8 or
  // k_24_8_FLOAT by its depth format).
  bool placeable = false;
  uint32_t dest_texture_base = 0;
  uint32_t dest_x_offset = 0;
  uint32_t dest_pitch_aligned = 0;
  uint32_t dest_bpp_log2 = 0;
  xenos::TextureFormat dest_texture_format = xenos::TextureFormat::k_8_8_8_8;
  // RB_COPY_DEST_INFO's; its first four values are xenos::Endian's.
  xenos::Endian128 dest_endian = xenos::Endian128::kNone;
  // The guest rectangles the copies wrote into the image since it was
  // created, in its coordinates.
  std::vector<Rect> written_rects;
  // Unique among the images of a run; serial: the resolve that last wrote
  // it (later ones are newer).
  uint64_t id = 0;
  uint64_t serial = 0;
};

// The host format of a colour resolve destination; VK_FORMAT_UNDEFINED for
// formats not handled yet (6_5_5, signed packed 16-bit: copies to them are
// dropped). 10_11_11 / 11_11_10 as RGBA16 unorm (the resolve pass converts);
// 1_5_5_5, 5_6_5 and 4_4_4_4 in the texture cache's host format for them
// (undefined without a texture cache).
VkFormat ResolveDestVkFormat(xenos::ColorFormat format, xenos::SurfaceNumberFormat number,
                             const NativeTextureCache* texture_cache);

class Resolver {
 public:
  Resolver() = default;
  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;
  ~Resolver();

  // With the shared memory, resolves are also written into it and CPU
  // writes to resolve destinations drop them (watched until Shutdown, which
  // comes before the shared memory's).
  bool Initialize(Submitter& submitter, RenderTargets& render_targets,
                  NativeSharedMemory* shared_memory);
  void Shutdown();
  // At 1x, resolves are also copied into the texture cache's textures of
  // their destinations, and images nothing reads aren't written
  // (native_resolve_into_textures). Null: neither. Until Shutdown.
  void SetTextureCache(NativeTextureCache* texture_cache) { texture_cache_ = texture_cache; }

  // frame_stats_interval: what resolves at 1x did with their images.
  struct Stats {
    // Of the resolves whose destination has a texture in the texture cache:
    // copied into it, or why not (by NativeTextureCache::ResolveCopy).
    static constexpr size_t kResultCount = 5;
    uint64_t into_textures[kResultCount] = {};
    // Not resolved into an image: no texture of it exists, and it isn't one
    // the present could show.
    uint64_t images_skipped = 0;
    // Copied from the render target into the texture without an image
    // (native_resolve_into_textures_direct; in into_textures too).
    uint64_t images_direct = 0;
  };
  Stats TakeStats() {
    Stats stats = stats_;
    stats_ = {};
    return stats;
  }

  // The guest's resolve (IssueCopy): the copy into the texture at
  // RB_COPY_DEST_BASE and the colour / depth clears that come with it, into
  // the submitter's open command buffer. memory_written_* get the range of
  // the shared memory the copy wrote the guest's bytes to (length 0: none).
  void Resolve(const RegisterFile& regs, const memory::Memory& memory, uint64_t frame,
               uint32_t& memory_written_start_out, uint32_t& memory_written_length_out);

  // The last texture resolved to `address` (physical, of any format), or
  // nullptr (also when the CPU has written to its memory since).
  ResolvedTexture* Find(uint32_t address);
  // The placeable images whose written memory overlaps [start, end)
  // (physical), oldest write first.
  void GetPlaceableOverlapping(uint32_t start, uint32_t end, std::vector<ResolvedTexture*>& out);
  // Changes whenever what GetPlaceableOverlapping and Find could return may
  // have: a resolve, images dropped for CPU writes (taken into account here).
  uint64_t generation() {
    DropWrittenByCpu();
    return generation_;
  }
  // Whether nothing GetPlaceableOverlapping could return for guest memory
  // [start, end) has changed since `generation` (images resolved to, replaced
  // or dropped elsewhere don't count). False when the changes since then are
  // no longer recorded.
  bool UnchangedSince(uint64_t generation, uint32_t start, uint32_t end);
  // Resolve images kept (frame_stats_interval).
  size_t texture_count() const { return textures_.size(); }
  // The last colour resolve of at least 640x360: what to present when the
  // front buffer address was not resolved to (AoT's fallback).
  uint32_t present_address() const { return present_address_; }

  void Transition(CommandRecorder* command_buffer, ResolvedTexture& texture, VkImageLayout layout);

  // A 2D array view for sampling the texture with the guest's host swizzle
  // (TextureCache's binding host_swizzle), its red and blue order taken into
  // account; the identity view without view swizzles on the device.
  VkImageView GetSampledView(ResolvedTexture& texture, uint32_t host_swizzle);

 private:
  ResolvedTexture* GetTexture(uint32_t address, xenos::ColorFormat guest_format,
                              VkFormat vk_format, uint32_t width, uint32_t height,
                              uint32_t scale4);
  // Taken out of the placeable index too.
  void DestroyLater(std::unique_ptr<ResolvedTexture> texture);
  // A new generation, and its entry in changes_ (no memory yet).
  void BeginChange();
  // The guest memory [start, end) is affected by the current generation's
  // change (nothing if empty).
  void NoteChange(uint32_t start, uint32_t end);
  // The texture's entry in placeable_index_ (none when not placeable or not
  // written), after its placement or written memory changed.
  void Unindex(const ResolvedTexture* texture);
  void Reindex(ResolvedTexture* texture);
  void LogOnce(uint32_t reason_bit, const char* message);
  // Records the range the resolve writes and marks it in the shared memory
  // (unless marked_start / marked_length, what the memory pass marked, cover
  // it).
  void MarkWrittenByGpu(const RegisterFile& regs, const memory::Memory& memory,
                        ResolvedTexture& texture, uint32_t marked_start, uint32_t marked_length);
  // Where the copy's pixels go in guest memory (ResolvedTexture::placeable
  // and the dest_* fields), and the rectangle it wrote.
  static void RecordPlacement(const RegisterFile& regs, const Rect& written,
                              ResolvedTexture& texture);
  // The shared memory's global watch, on the thread writing (within its
  // global critical region): CPU writes queued for DropWrittenByCpu.
  static void MemoryWrittenThunk(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, uint32_t address_first, uint32_t address_last,
                                 bool invalidated_by_gpu);
  // Destroys the textures the queued CPU writes overlap (command processor
  // thread).
  void DropWrittenByCpu();

  Submitter* submitter_ = nullptr;
  RenderTargets* render_targets_ = nullptr;
  NativeSharedMemory* shared_memory_ = nullptr;
  NativeTextureCache* texture_cache_ = nullptr;
  Stats stats_;
  // Resolves into the shared memory; without it, guest memory isn't written.
  ResolveMemory memory_pass_;
  bool memory_pass_ready_ = false;
  // Resolves needing a conversion; without it they are dropped (as before).
  ResolvePass pass_;
  bool pass_ready_ = false;
  void* memory_watch_ = nullptr;
  // Written ranges (first, last), from the watch.
  std::mutex cpu_writes_mutex_;
  std::vector<std::pair<uint32_t, uint32_t>> cpu_writes_;
  std::atomic<bool> cpu_writes_pending_{false};
  // By address and host format (ResolveImageKey in resolve.cpp).
  std::unordered_map<uint64_t, std::unique_ptr<ResolvedTexture>> textures_;
  uint32_t present_address_ = UINT32_MAX;
  uint32_t logged_ = 0;
  uint64_t next_texture_id_ = 1;
  uint64_t next_serial_ = 1;
  uint64_t generation_ = 1;
  // The guest memory each generation changed, oldest first, one entry a
  // generation (a resolve's: its destination's old and new written memory;
  // a dropped image's: its memory), the last kMaxChanges kept: memos of
  // lookups in other memory stay valid (UnchangedSince). Draws look up
  // thousands of textures a frame, between 70 or more resolves.
  struct Change {
    uint64_t generation;
    uint32_t start;
    uint32_t end;
  };
  static constexpr size_t kMaxChanges = 1024;
  std::deque<Change> changes_;
  // GetPlaceableOverlapping's index of the placeable images, by the start of
  // their written memory, kept sorted as images are written or dropped (it
  // was rebuilt and sorted for each new generation, with 150-190 images).
  struct PlaceableEntry {
    uint32_t start;
    uint32_t end;
    ResolvedTexture* texture;
  };
  std::vector<PlaceableEntry> placeable_index_;
  // The largest end among placeable_index_[0..i]: a backwards scan from the
  // last start below a query's end stops where nothing earlier reaches it.
  // Recomputed at the first query after the index changed.
  std::vector<uint32_t> placeable_max_end_;
  bool placeable_max_end_dirty_ = false;
};

}  // namespace rex::graphics::native
