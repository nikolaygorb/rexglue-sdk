/**
 * @file        fuzz/primitive_processor_fuzzer.cpp
 * @brief       libFuzzer target for the primitive processor: guest primitive
 *              types, index buffers and primitive reset to what the host draws
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include <rex/graphics/primitive_processor.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/memory/utils.h>
#include <rex/system/xmemory.h>

using namespace rex::graphics;

// Input layout:
//   0: bit 0      - the host supports full 32-bit indices;
//      bit 1      - the host supports triangle fans;
//      bit 2      - the host supports line loops;
//      bit 3      - the host supports quad lists;
//      bit 4      - the host supports point sprites without vertex shader
//                   expansion;
//      bit 5      - the host supports rectangle lists without vertex shader
//                   expansion;
//      bit 6      - the host cuts strips at an all-ones index even without
//                   primitive reset (Direct3D 11);
//   1..24         - VGT_DRAW_INITIATOR, VGT_DMA_SIZE, PA_SU_SC_MODE_CNTL,
//                   VGT_MULTI_PRIM_IB_RESET_INDX, VGT_OUTPUT_PATH_CNTL,
//                   VGT_HOS_CNTL (little endian, XOR an 8-index 16-bit
//                   triangle strip with primitive reset at 0xFFFF);
//   then          - the index buffer as in guest memory, repeated to the size
//                   the draw needs (0 if empty).
// Direct3D 11 is 0x79 (32-bit indices, quads, points and rectangles, cuts at
// all-ones), Direct3D 12 0x39 (no cuts) and Vulkan 0x39 or less.
// VGT_DMA_BASE is the fuzzer's.
//
// PrimitiveProcessor::Process runs on the registers, as before a draw, with a
// backend that keeps its index buffers in host memory. Checked on top of ASan:
// - the guest's indices end where the draw says, at a page the fuzzer made
//   inaccessible, so reading past them crashes;
// - converted index buffers are written only inside what was requested;
// - the host index buffer (built-in, converted, or the guest's, then requested
//   in the shared memory) holds all the indices the host draws;
// - primitive reset is never enabled for lists;
// - the host draws the same points, lines and triangles as the guest: both
//   index streams are decoded with the endian swap, the reset index and the
//   strip cuts that apply to them, and compared as multisets of primitives
//   (not for tessellation and the point and rectangle expansion).

namespace {

constexpr uint32_t kPageSize = 4096;
// 0xFFFF 32-bit indices at most.
constexpr uint32_t kIndexBufferMaxBytes = 0x40000;
constexpr uint32_t kIndexSlotSize = kPageSize + kIndexBufferMaxBytes + kPageSize;
// The primitive comparison sorts all of them; above this only the bounds are
// checked.
constexpr uint32_t kCompareMaxIndices = 16384;

class FuzzSharedMemory final : public SharedMemory {
 public:
  explicit FuzzSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override {
    return true;
  }
};

class FuzzPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  struct Converted {
    uint8_t* allocation;
    size_t allocation_size;
    uint8_t* mapping;
    size_t size_bytes;
    xenos::IndexFormat format;
    uint32_t index_count;
  };

  static constexpr uint8_t kCanary = 0xCD;

  FuzzPrimitiveProcessor(const RegisterFile& register_file, rex::memory::Memory& memory,
                         SharedMemory& shared_memory)
      : PrimitiveProcessor(register_file, memory, shared_memory) {}
  ~FuzzPrimitiveProcessor() override {
    ReleaseConverted();
    ShutdownCommon();
  }

  bool Initialize(uint8_t capabilities) {
    return InitializeCommon(capabilities & (1 << 0), capabilities & (1 << 1),
                            capabilities & (1 << 2), capabilities & (1 << 3),
                            capabilities & (1 << 4), capabilities & (1 << 5));
  }

  // A new frame: nothing converted, nothing cached.
  void BeginDraw() {
    ReleaseConverted();
    ClearPerFrameCache();
  }

  const std::vector<Converted>& converted() const { return converted_; }
  const std::vector<uint8_t>& builtin() const { return builtin_; }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override {
    builtin_.resize(size_bytes);
    fill_callback(builtin_.data());
    return true;
  }

  // Like the Direct3D 11 backend, but an allocation of its own for each, filled
  // with kCanary.
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override {
    size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
    Converted converted;
    converted.size_bytes = index_size * index_count;
    converted.allocation_size =
        converted.size_bytes + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
    converted.allocation = static_cast<uint8_t*>(
        ::operator new[](converted.allocation_size, std::align_val_t(16)));
    std::memset(converted.allocation, kCanary, converted.allocation_size);
    converted.mapping = converted.allocation;
    if (coalign_for_simd) {
      converted.mapping += GetSimdCoalignmentOffset(converted.mapping, coalignment_original_address);
    }
    converted.format = format;
    converted.index_count = index_count;
    backend_handle_out = converted_.size();
    converted_.push_back(converted);
    return converted.mapping;
  }

 private:
  void ReleaseConverted() {
    for (const Converted& converted : converted_) {
      ::operator delete[](converted.allocation, std::align_val_t(16));
    }
    converted_.clear();
  }

  std::vector<uint8_t> builtin_;
  std::vector<Converted> converted_;
};

struct Guest {
  rex::memory::Memory memory;
  std::unique_ptr<RegisterFile> regs = std::make_unique<RegisterFile>();
  std::unique_ptr<FuzzSharedMemory> shared_memory;
  std::array<std::unique_ptr<FuzzPrimitiveProcessor>, 64> processors;
  uint32_t index_slot_physical = 0;

  // Where the index buffer ends: the guard page.
  uint32_t IndexBufferEnd() const { return index_slot_physical + kPageSize + kIndexBufferMaxBytes; }
};

void Guard(Guest& guest, uint32_t physical) {
  if (!rex::memory::Protect(guest.memory.TranslatePhysical(physical), kPageSize,
                            rex::memory::PageAccess::kNoAccess)) {
    std::fprintf(stderr, "primitive_processor_fuzzer: can't protect a guard page\n");
    std::abort();
  }
}

Guest* GetGuest() {
  static Guest* guest = [] {
    auto* guest = new Guest;
    if (!guest->memory.Initialize()) {
      std::fprintf(stderr, "primitive_processor_fuzzer: can't reserve the guest address space\n");
      std::abort();
    }
    uint32_t slot_address =
        guest->memory.SystemHeapAlloc(kIndexSlotSize, 0x10000, rex::memory::kSystemHeapPhysical);
    if (!slot_address) {
      std::fprintf(stderr, "primitive_processor_fuzzer: can't allocate guest memory\n");
      std::abort();
    }
    guest->index_slot_physical = guest->memory.GetPhysicalAddress(slot_address);
    Guard(*guest, guest->index_slot_physical);
    Guard(*guest, guest->IndexBufferEnd());
    guest->shared_memory = std::make_unique<FuzzSharedMemory>(guest->memory);
    return guest;
  }();
  return guest;
}

struct RegisterDefault {
  uint32_t index;
  uint32_t value;
};

std::array<RegisterDefault, 6> MakeRegisters() {
  reg::VGT_DRAW_INITIATOR draw_initiator = {};
  draw_initiator.prim_type = xenos::PrimitiveType::kTriangleStrip;
  draw_initiator.source_select = xenos::SourceSelect::kDMA;
  draw_initiator.index_size = xenos::IndexFormat::kInt16;
  draw_initiator.num_indices = 8;
  reg::VGT_DMA_SIZE dma_size = {};
  dma_size.num_words = 8;
  dma_size.swap_mode = xenos::Endian::k8in16;
  reg::PA_SU_SC_MODE_CNTL mode_control = {};
  mode_control.multi_prim_ib_ena = 1;
  reg::VGT_MULTI_PRIM_IB_RESET_INDX reset_index = {};
  reset_index.reset_indx = 0xFFFF;
  return {{
      {reg::VGT_DRAW_INITIATOR::register_index, draw_initiator.value},
      {reg::VGT_DMA_SIZE::register_index, dma_size.value},
      {reg::PA_SU_SC_MODE_CNTL::register_index, mode_control.value},
      {reg::VGT_MULTI_PRIM_IB_RESET_INDX::register_index, reset_index.value},
      {reg::VGT_OUTPUT_PATH_CNTL::register_index, 0},
      {reg::VGT_HOS_CNTL::register_index, 0},
  }};
}

// Primitive comparison.

constexpr int64_t kCut = -1;

struct Primitive {
  uint32_t kind;  // Vertex count, or 4 for a rectangle.
  std::array<uint32_t, 3> vertices;
  bool operator<(const Primitive& other) const {
    return kind != other.kind ? kind < other.kind : vertices < other.vertices;
  }
  bool operator==(const Primitive& other) const {
    return kind == other.kind && vertices == other.vertices;
  }
};

void AddPrimitive(std::vector<Primitive>& out, uint32_t kind, uint32_t v0, uint32_t v1 = 0,
                  uint32_t v2 = 0) {
  Primitive primitive{kind, {v0, v1, v2}};
  uint32_t vertex_count = kind == 4 ? 3 : kind;
  std::sort(primitive.vertices.begin(), primitive.vertices.begin() + vertex_count);
  out.push_back(primitive);
}

// Appends the primitives of a stream of vertices and cuts; false for types the
// comparison doesn't know.
bool Decode(xenos::PrimitiveType type, const std::vector<int64_t>& stream,
            std::vector<Primitive>& out) {
  size_t run_start = 0;
  while (run_start <= stream.size()) {
    size_t run_end = run_start;
    while (run_end < stream.size() && stream[run_end] != kCut) {
      ++run_end;
    }
    const int64_t* run = stream.data() + run_start;
    size_t n = run_end - run_start;
    auto v = [&](size_t i) { return uint32_t(run[i]); };
    switch (type) {
      case xenos::PrimitiveType::kPointList:
        for (size_t i = 0; i < n; ++i) {
          AddPrimitive(out, 1, v(i));
        }
        break;
      case xenos::PrimitiveType::kLineList:
        for (size_t i = 0; i + 1 < n; i += 2) {
          AddPrimitive(out, 2, v(i), v(i + 1));
        }
        break;
      case xenos::PrimitiveType::kLineStrip:
        for (size_t i = 1; i < n; ++i) {
          AddPrimitive(out, 2, v(i - 1), v(i));
        }
        break;
      case xenos::PrimitiveType::kLineLoop:
        for (size_t i = 1; i < n; ++i) {
          AddPrimitive(out, 2, v(i - 1), v(i));
        }
        if (n > 1) {
          AddPrimitive(out, 2, v(n - 1), v(0));
        }
        break;
      case xenos::PrimitiveType::kTriangleList:
        for (size_t i = 0; i + 2 < n; i += 3) {
          AddPrimitive(out, 3, v(i), v(i + 1), v(i + 2));
        }
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        for (size_t i = 2; i < n; ++i) {
          AddPrimitive(out, 3, v(i - 2), v(i - 1), v(i));
        }
        break;
      case xenos::PrimitiveType::kTriangleFan:
        for (size_t i = 2; i < n; ++i) {
          AddPrimitive(out, 3, v(i - 1), v(i), v(0));
        }
        break;
      case xenos::PrimitiveType::kQuadList:
        // As PrimitiveProcessor::QuadListToTriangleList splits them.
        for (size_t i = 0; i + 3 < n; i += 4) {
          AddPrimitive(out, 3, v(i), v(i + 1), v(i + 2));
          AddPrimitive(out, 3, v(i), v(i + 2), v(i + 3));
        }
        break;
      case xenos::PrimitiveType::kRectangleList:
        for (size_t i = 0; i + 2 < n; i += 3) {
          AddPrimitive(out, 4, v(i), v(i + 1), v(i + 2));
        }
        break;
      default:
        return false;
    }
    run_start = run_end + 1;
  }
  return true;
}

bool IsStrip(xenos::PrimitiveType type) {
  return type == xenos::PrimitiveType::kLineStrip || type == xenos::PrimitiveType::kTriangleStrip;
}

bool IsList(xenos::PrimitiveType type) {
  switch (type) {
    case xenos::PrimitiveType::kPointList:
    case xenos::PrimitiveType::kLineList:
    case xenos::PrimitiveType::kTriangleList:
    case xenos::PrimitiveType::kRectangleList:
    case xenos::PrimitiveType::kQuadList:
      return true;
    default:
      return false;
  }
}

// The types for which VGT_MULTI_PRIM_IB_RESET_INDX applies (the same as in
// PrimitiveProcessor::Process).
bool IsResettable(xenos::PrimitiveType type) {
  switch (type) {
    case xenos::PrimitiveType::kLineStrip:
    case xenos::PrimitiveType::kTriangleFan:
    case xenos::PrimitiveType::kTriangleStrip:
    case xenos::PrimitiveType::kLineLoop:
    case xenos::PrimitiveType::kQuadStrip:
    case xenos::PrimitiveType::kPolygon:
    case xenos::PrimitiveType::k2DLineStrip:
    case xenos::PrimitiveType::k2DTriStrip:
      return true;
    default:
      return false;
  }
}

struct Draw {
  uint8_t capabilities;
  reg::VGT_DRAW_INITIATOR initiator;
  reg::VGT_DMA_SIZE dma_size;
  bool reset_enabled;
  uint32_t reset_index;
  uint32_t index_base;
  uint32_t index_count;
  const uint8_t* indices;
};

[[noreturn]] void Fail(const char* what, const Draw& draw,
                       const PrimitiveProcessor::ProcessingResult& result) {
  std::fprintf(stderr,
               "primitive_processor_fuzzer: %s\n"
               "  capabilities 0x%02X; guest: type 0x%02X, %u %s indices (%s), endian %u, reset "
               "%s 0x%06X;\n"
               "  host: type 0x%02X, %u indices, buffer type %d, %s, endian %u, reset %s, "
               "handle %zu\n",
               what, draw.capabilities, uint32_t(draw.initiator.prim_type), draw.index_count,
               draw.initiator.index_size == xenos::IndexFormat::kInt16 ? "16-bit" : "32-bit",
               draw.initiator.source_select == xenos::SourceSelect::kDMA ? "DMA" : "auto",
               uint32_t(draw.dma_size.swap_mode), draw.reset_enabled ? "on" : "off",
               draw.reset_index, uint32_t(result.host_primitive_type),
               result.host_draw_vertex_count, int(result.index_buffer_type),
               result.host_index_format == xenos::IndexFormat::kInt16 ? "16-bit" : "32-bit",
               uint32_t(result.host_shader_index_endian),
               result.host_primitive_reset_enabled ? "on" : "off", result.host_index_buffer_handle);
  std::abort();
}

// The guest's vertices and cuts.
void GuestStream(const Draw& draw, std::vector<int64_t>& out) {
  out.clear();
  if (draw.initiator.source_select == xenos::SourceSelect::kAutoIndex) {
    for (uint32_t i = 0; i < draw.index_count; ++i) {
      out.push_back(i);
    }
    return;
  }
  xenos::Endian endian = draw.dma_size.swap_mode;
  if (draw.initiator.index_size == xenos::IndexFormat::kInt16) {
    // 32-bit swaps aren't for 16-bit indices; Process takes 8-in-32 for 8-in-16
    // and 16-in-32 for none.
    if (endian == xenos::Endian::k8in32) {
      endian = xenos::Endian::k8in16;
    } else if (endian == xenos::Endian::k16in32) {
      endian = xenos::Endian::kNone;
    }
    for (uint32_t i = 0; i < draw.index_count; ++i) {
      uint16_t raw;
      std::memcpy(&raw, draw.indices + i * sizeof(raw), sizeof(raw));
      uint32_t index = xenos::GpuSwap(raw, endian);
      out.push_back(draw.reset_enabled && index == draw.reset_index ? kCut : int64_t(index));
    }
  } else {
    for (uint32_t i = 0; i < draw.index_count; ++i) {
      uint32_t raw;
      std::memcpy(&raw, draw.indices + i * sizeof(raw), sizeof(raw));
      // Only the lower 24 bits are the vertex, and only they are compared to
      // the reset index.
      uint32_t index = xenos::GpuSwap(raw, endian) & xenos::kVertexIndexMask;
      out.push_back(draw.reset_enabled && index == draw.reset_index ? kCut : int64_t(index));
    }
  }
}

// The host's vertices and cuts from an index buffer.
void HostStream(const uint8_t* indices, const PrimitiveProcessor::ProcessingResult& result,
                bool cut_strips_always, std::vector<int64_t>& out) {
  out.clear();
  bool cuts = result.host_primitive_reset_enabled ||
              (cut_strips_always && IsStrip(result.host_primitive_type));
  bool is_16bit = result.host_index_format == xenos::IndexFormat::kInt16;
  for (uint32_t i = 0; i < result.host_draw_vertex_count; ++i) {
    uint32_t raw;
    if (is_16bit) {
      uint16_t raw_16;
      std::memcpy(&raw_16, indices + i * sizeof(raw_16), sizeof(raw_16));
      raw = raw_16;
    } else {
      std::memcpy(&raw, indices + i * sizeof(raw), sizeof(raw));
    }
    if (cuts && raw == (is_16bit ? UINT16_MAX : UINT32_MAX)) {
      out.push_back(kCut);
      continue;
    }
    out.push_back(xenos::GpuSwap(raw, result.host_shader_index_endian) & xenos::kVertexIndexMask);
  }
}

void ComparePrimitives(const Draw& draw, const PrimitiveProcessor::ProcessingResult& result,
                       const FuzzPrimitiveProcessor& processor) {
  if (result.IsTessellated() ||
      result.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex ||
      draw.index_count > kCompareMaxIndices) {
    return;
  }
  static std::vector<int64_t> guest_stream, host_stream;
  static std::vector<Primitive> guest_primitives, host_primitives;
  GuestStream(draw, guest_stream);
  bool cut_strips_always = (draw.capabilities & (1 << 6)) != 0;
  switch (result.index_buffer_type) {
    case PrimitiveProcessor::kNone:
      // Auto-indexed; the added closing index of a line loop is vertex 0.
      host_stream.clear();
      for (uint32_t i = 0; i < result.host_draw_vertex_count; ++i) {
        host_stream.push_back(i == result.line_loop_closing_index ? 0 : i);
      }
      break;
    case PrimitiveProcessor::kGuestDMA:
      HostStream(draw.indices, result, cut_strips_always, host_stream);
      break;
    case PrimitiveProcessor::kHostConverted:
      HostStream(processor.converted()[result.host_index_buffer_handle].mapping, result,
                 cut_strips_always, host_stream);
      break;
    case PrimitiveProcessor::kHostBuiltinForAuto:
      HostStream(processor.builtin().data() + result.host_index_buffer_handle, result,
                 cut_strips_always, host_stream);
      break;
    default:
      return;
  }
  guest_primitives.clear();
  host_primitives.clear();
  if (!Decode(draw.initiator.prim_type, guest_stream, guest_primitives) ||
      !Decode(result.host_primitive_type, host_stream, host_primitives)) {
    return;
  }
  std::sort(guest_primitives.begin(), guest_primitives.end());
  std::sort(host_primitives.begin(), host_primitives.end());
  if (guest_primitives == host_primitives) {
    return;
  }
  auto mismatch = std::mismatch(guest_primitives.begin(), guest_primitives.end(),
                                host_primitives.begin(), host_primitives.end());
  auto print = [](const char* side, const std::vector<Primitive>& primitives,
                  std::vector<Primitive>::const_iterator it) {
    if (it == primitives.end()) {
      std::fprintf(stderr, "  %s: %zu primitives, nothing more\n", side, primitives.size());
    } else {
      std::fprintf(stderr, "  %s: %zu primitives, then kind %u (%u, %u, %u)\n", side,
                   primitives.size(), it->kind, it->vertices[0], it->vertices[1], it->vertices[2]);
    }
  };
  print("guest", guest_primitives, mismatch.first);
  print("host", host_primitives, mismatch.second);
  Fail("the host draws other primitives than the guest", draw, result);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static const std::array<RegisterDefault, 6> registers = MakeRegisters();
  constexpr size_t kHeaderSize = 1 + sizeof(uint32_t) * registers.size();
  if (size < kHeaderSize) {
    return 0;
  }
  Guest* guest = GetGuest();
  RegisterFile& regs = *guest->regs;

  Draw draw = {};
  draw.capabilities = data[0] & 0x7F;
  std::unique_ptr<FuzzPrimitiveProcessor>& processor = guest->processors[draw.capabilities & 63];
  if (!processor) {
    processor = std::make_unique<FuzzPrimitiveProcessor>(regs, guest->memory,
                                                         *guest->shared_memory);
    if (!processor->Initialize(draw.capabilities)) {
      std::fprintf(stderr, "primitive_processor_fuzzer: can't initialize the processor\n");
      std::abort();
    }
  }
  processor->SetHostStripPrimitiveResetAlwaysEnabled((draw.capabilities & (1 << 6)) != 0);

  std::memset(regs.values, 0, sizeof(regs.values));
  for (size_t i = 0; i < registers.size(); ++i) {
    uint32_t value;
    std::memcpy(&value, data + 1 + sizeof(uint32_t) * i, sizeof(value));
    regs[registers[i].index] = value ^ registers[i].value;
  }
  data += kHeaderSize;
  size -= kHeaderSize;

  // The indices the draw reads, as Process works it out, at the end of the
  // index buffer.
  draw.initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  draw.dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  draw.index_count = draw.initiator.num_indices;
  uint32_t index_bytes = 0;
  if (draw.initiator.source_select == xenos::SourceSelect::kDMA) {
    draw.index_count = std::min(draw.index_count, uint32_t(draw.dma_size.num_words));
    index_bytes = draw.index_count *
                  (draw.initiator.index_size == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                                            : sizeof(uint32_t));
  }
  draw.index_base = guest->IndexBufferEnd() - index_bytes;
  regs[XE_GPU_REG_VGT_DMA_BASE] = draw.index_base;
  uint8_t* indices = guest->memory.TranslatePhysical(draw.index_base);
  for (uint32_t i = 0; i < index_bytes; ++i) {
    indices[i] = size ? data[i % size] : 0;
  }
  draw.indices = indices;
  uint32_t reset_index = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  draw.reset_enabled = regs.Get<reg::PA_SU_SC_MODE_CNTL>().multi_prim_ib_ena &&
                       IsResettable(draw.initiator.prim_type) &&
                       (draw.initiator.index_size == xenos::IndexFormat::kInt32 ||
                        reset_index <= UINT16_MAX);
  draw.reset_index = reset_index;

  processor->BeginDraw();
  PrimitiveProcessor::ProcessingResult result;
  if (!processor->Process(result)) {
    return 0;
  }

  // Converted index buffers: nothing written around them.
  for (const FuzzPrimitiveProcessor::Converted& converted : processor->converted()) {
    for (const uint8_t* byte = converted.allocation; byte < converted.mapping; ++byte) {
      if (*byte != FuzzPrimitiveProcessor::kCanary) {
        Fail("written in front of a converted index buffer", draw, result);
      }
    }
    for (const uint8_t* byte = converted.mapping + converted.size_bytes;
         byte < converted.allocation + converted.allocation_size; ++byte) {
      if (*byte != FuzzPrimitiveProcessor::kCanary) {
        Fail("written past the end of a converted index buffer", draw, result);
      }
    }
  }

  if (result.host_primitive_reset_enabled && IsList(result.host_primitive_type)) {
    Fail("primitive reset enabled for a list", draw, result);
  }

  // The host index buffer holds what the host draws.
  size_t host_index_size =
      result.host_index_format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
  uint64_t host_bytes = uint64_t(result.host_draw_vertex_count) * host_index_size;
  std::pair<uint32_t, uint32_t> requested = processor->last_guest_index_range();
  switch (result.index_buffer_type) {
    case PrimitiveProcessor::kNone:
      break;
    case PrimitiveProcessor::kGuestDMA:
      if (result.guest_index_base != draw.index_base || requested.first != draw.index_base ||
          requested.second < host_bytes) {
        Fail("the host draws guest indices not requested in the shared memory", draw, result);
      }
      if (host_bytes > index_bytes) {
        Fail("the host draws past the guest's index buffer", draw, result);
      }
      break;
    case PrimitiveProcessor::kHostConverted: {
      if (result.host_index_buffer_handle >= processor->converted().size()) {
        Fail("the host draws a converted index buffer that doesn't exist", draw, result);
      }
      const FuzzPrimitiveProcessor::Converted& converted =
          processor->converted()[result.host_index_buffer_handle];
      if (converted.format != result.host_index_format) {
        Fail("the host draws a converted index buffer in another format", draw, result);
      }
      if (converted.index_count < result.host_draw_vertex_count) {
        Fail("the host draws past the end of a converted index buffer", draw, result);
      }
    } break;
    case PrimitiveProcessor::kHostBuiltinForAuto:
    case PrimitiveProcessor::kHostBuiltinForDMA:
      if (result.host_index_buffer_handle > processor->builtin().size() ||
          processor->builtin().size() - result.host_index_buffer_handle < host_bytes) {
        Fail("the host draws past the end of the built-in index buffer", draw, result);
      }
      if (result.index_buffer_type == PrimitiveProcessor::kHostBuiltinForDMA &&
          (requested.first != draw.index_base || requested.second < index_bytes)) {
        Fail("the vertex shader reads guest indices not requested in the shared memory", draw,
             result);
      }
      break;
  }

  ComparePrimitives(draw, result, *processor);
  return 0;
}
