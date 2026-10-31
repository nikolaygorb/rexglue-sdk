/**
 * @file        graphics/primitive_reset_test.cpp
 * @brief       Tests for what the host cuts at: primitive reset, strips with
 *              an all-ones vertex, line loops converted for the host
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
#include <rex/system/xmemory.h>

#include "test_memory.h"

namespace {

using namespace rex::graphics;

class TestSharedMemory final : public SharedMemory {
 public:
  explicit TestSharedMemory(rex::memory::Memory& memory) : SharedMemory(memory) {
    InitializeCommon();
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override {
    return true;
  }
};

class TestPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  TestPrimitiveProcessor(const RegisterFile& register_file, rex::memory::Memory& memory,
                         SharedMemory& shared_memory)
      : PrimitiveProcessor(register_file, memory, shared_memory) {}
  ~TestPrimitiveProcessor() override {
    ReleaseConverted();
    ShutdownCommon();
  }

  // Full 32-bit indices and no line loops, as on Direct3D 11 and 12.
  bool Initialize() { return InitializeCommon(true, false, false, true, true, true); }

  void BeginDraw() {
    ReleaseConverted();
    ClearPerFrameCache();
  }

  // The indices of the only converted index buffer of the draw.
  template <typename Index>
  std::vector<uint32_t> Converted(uint32_t count) const {
    REQUIRE(converted_.size() == 1);
    auto indices = reinterpret_cast<const Index*>(converted_[0].mapping);
    return std::vector<uint32_t>(indices, indices + count);
  }

  // The indices of the last converted index buffer of the frame.
  template <typename Index>
  std::vector<uint32_t> LastConverted(uint32_t count) const {
    REQUIRE_FALSE(converted_.empty());
    auto indices = reinterpret_cast<const Index*>(converted_.back().mapping);
    return std::vector<uint32_t>(indices, indices + count);
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override {
    builtin_.resize(size_bytes);
    fill_callback(builtin_.data());
    return true;
  }

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override {
    size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
    size_t size = index_size * index_count + XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE;
    Buffer buffer;
    buffer.allocation = static_cast<uint8_t*>(::operator new[](size, std::align_val_t(16)));
    buffer.mapping = buffer.allocation;
    if (coalign_for_simd) {
      buffer.mapping += GetSimdCoalignmentOffset(buffer.mapping, coalignment_original_address);
    }
    backend_handle_out = converted_.size();
    converted_.push_back(buffer);
    return buffer.mapping;
  }

 private:
  struct Buffer {
    uint8_t* allocation;
    uint8_t* mapping;
  };

  void ReleaseConverted() {
    for (const Buffer& buffer : converted_) {
      ::operator delete[](buffer.allocation, std::align_val_t(16));
    }
    converted_.clear();
  }

  std::vector<uint8_t> builtin_;
  std::vector<Buffer> converted_;
};

constexpr uint32_t kNoReset = UINT32_MAX;
constexpr uint32_t kIndexBufferBytes = 4096;

// A processor with the registers and an index buffer in guest memory.
struct Host {
  explicit Host(bool strip_cuts_always)
      : memory(rex::testing::GetTestMemory()),
        regs(std::make_unique<RegisterFile>()),
        shared_memory(memory),
        processor(*regs, memory, shared_memory) {
    processor.SetHostStripPrimitiveResetAlwaysEnabled(strip_cuts_always);
    REQUIRE(processor.Initialize());
    indices_address = memory.SystemHeapAlloc(kIndexBufferBytes, kIndexBufferBytes,
                                             rex::memory::kSystemHeapPhysical);
    REQUIRE(indices_address);
  }
  ~Host() { memory.SystemHeapFree(indices_address); }

  // Draws without an endian swap, so the guest's indices are as written here.
  template <typename Index>
  PrimitiveProcessor::ProcessingResult Draw(xenos::PrimitiveType type,
                                            const std::vector<Index>& indices,
                                            uint32_t reset_index) {
    uint32_t physical = memory.GetPhysicalAddress(indices_address);
    std::memcpy(memory.TranslatePhysical(physical), indices.data(),
                indices.size() * sizeof(Index));
    std::memset(regs->values, 0, sizeof(regs->values));
    reg::VGT_DRAW_INITIATOR draw_initiator = {};
    draw_initiator.prim_type = type;
    draw_initiator.source_select = xenos::SourceSelect::kDMA;
    draw_initiator.index_size =
        sizeof(Index) == 2 ? xenos::IndexFormat::kInt16 : xenos::IndexFormat::kInt32;
    draw_initiator.num_indices = uint32_t(indices.size());
    reg::VGT_DMA_SIZE dma_size = {};
    dma_size.num_words = uint32_t(indices.size());
    dma_size.swap_mode = xenos::Endian::kNone;
    reg::PA_SU_SC_MODE_CNTL mode_control = {};
    mode_control.multi_prim_ib_ena = reset_index != kNoReset;
    reg::VGT_MULTI_PRIM_IB_RESET_INDX reset = {};
    reset.reset_indx = reset_index != kNoReset ? reset_index : 0;
    (*regs)[reg::VGT_DRAW_INITIATOR::register_index] = draw_initiator.value;
    (*regs)[reg::VGT_DMA_SIZE::register_index] = dma_size.value;
    (*regs)[reg::PA_SU_SC_MODE_CNTL::register_index] = mode_control.value;
    (*regs)[reg::VGT_MULTI_PRIM_IB_RESET_INDX::register_index] = reset.value;
    (*regs)[XE_GPU_REG_VGT_DMA_BASE] = physical;
    processor.BeginDraw();
    PrimitiveProcessor::ProcessingResult result;
    REQUIRE(processor.Process(result));
    return result;
  }

  // The same buffer again in the same frame, with another reset index.
  PrimitiveProcessor::ProcessingResult DrawAgain(uint32_t reset_index) {
    reg::VGT_MULTI_PRIM_IB_RESET_INDX reset = {};
    reset.reset_indx = reset_index;
    (*regs)[reg::VGT_MULTI_PRIM_IB_RESET_INDX::register_index] = reset.value;
    PrimitiveProcessor::ProcessingResult result;
    REQUIRE(processor.Process(result));
    return result;
  }

  rex::memory::Memory& memory;
  std::unique_ptr<RegisterFile> regs;
  TestSharedMemory shared_memory;
  TestPrimitiveProcessor processor;
  uint32_t indices_address = 0;
};

using Indices16 = std::vector<uint16_t>;
using Indices32 = std::vector<uint32_t>;
using Expected = std::vector<uint32_t>;

}  // namespace

TEST_CASE("An unused reset index doesn't cut at an all-ones vertex", "[graphics][primitive]") {
  Host host(false);

  SECTION("16-bit") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices16{0, 1, 0xFFFF, 2}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kGuestDMA);
    CHECK_FALSE(result.host_primitive_reset_enabled);
  }
  SECTION("32-bit") {
    auto result = host.Draw<uint32_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices32{0, 1, 0xFFFFFFFF, 2}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kGuestDMA);
    CHECK_FALSE(result.host_primitive_reset_enabled);
  }
}

TEST_CASE("Primitive reset stays on when the host can't cut at a vertex",
          "[graphics][primitive]") {
  Host host(false);

  SECTION("The reset index is used") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices16{0, 1, 2, 0x1234, 3, 4, 5}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kHostConverted);
    CHECK(result.host_primitive_reset_enabled);
    CHECK(host.processor.Converted<uint16_t>(7) == Expected{0, 1, 2, 0xFFFF, 3, 4, 5});
  }
  SECTION("No all-ones vertex, reset index not used") {
    auto result =
        host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip, Indices16{0, 1, 2}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kGuestDMA);
    CHECK(result.host_primitive_reset_enabled);
  }
  SECTION("The reset index is all-ones") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices16{0, 1, 2, 0xFFFF, 3, 4, 5}, 0xFFFF);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kGuestDMA);
    CHECK(result.host_primitive_reset_enabled);
  }
}

TEST_CASE("A strip with an unused reset index survives hosts that always cut strips",
          "[graphics][primitive]") {
  Host host(true);

  SECTION("16-bit") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices16{0, 1, 0xFFFF, 2}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kHostConverted);
    CHECK(result.host_index_format == xenos::IndexFormat::kInt32);
    CHECK_FALSE(result.host_primitive_reset_enabled);
    CHECK(host.processor.Converted<uint32_t>(4) == Expected{0, 1, 0xFFFF, 2});
  }
  SECTION("32-bit") {
    auto result = host.Draw<uint32_t>(xenos::PrimitiveType::kTriangleStrip,
                                      Indices32{0, 1, 0xFFFFFFFF, 2}, 0x1234);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kHostConverted);
    CHECK_FALSE(result.host_primitive_reset_enabled);
    CHECK(host.processor.Converted<uint32_t>(4) == Expected{0, 1, 0xFFFFFF, 2});
  }
}

TEST_CASE("One line loop stays a line strip", "[graphics][primitive]") {
  Host host(false);
  auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop, Indices16{4, 5, 6}, kNoReset);
  CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineStrip);
  REQUIRE(result.host_draw_vertex_count == 4);
  CHECK(host.processor.Converted<uint16_t>(4) == Expected{4, 5, 6, 4});
}

TEST_CASE("Line loops separated by reset are not joined", "[graphics][primitive]") {
  Host host(false);

  SECTION("16-bit") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop,
                                      Indices16{1, 2, 3, 0xFFFF, 7, 8}, 0xFFFF);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
    CHECK_FALSE(result.host_primitive_reset_enabled);
    REQUIRE(result.host_draw_vertex_count == 10);
    CHECK(host.processor.Converted<uint16_t>(10) == Expected{1, 2, 2, 3, 3, 1, 7, 8, 8, 7});
  }
  SECTION("32-bit") {
    auto result = host.Draw<uint32_t>(xenos::PrimitiveType::kLineLoop,
                                      Indices32{1, 2, 3, 0x1234, 7, 8}, 0x1234);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
    REQUIRE(result.host_draw_vertex_count == 10);
    CHECK(host.processor.Converted<uint32_t>(10) == Expected{1, 2, 2, 3, 3, 1, 7, 8, 8, 7});
  }
  SECTION("A loop of one vertex between them draws nothing") {
    auto result = host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop,
                                      Indices16{1, 2, 0xFFFF, 5, 0xFFFF, 7, 8}, 0xFFFF);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
    REQUIRE(result.host_draw_vertex_count == 8);
    CHECK(host.processor.Converted<uint16_t>(8) == Expected{1, 2, 2, 1, 7, 8, 8, 7});
  }
}

TEST_CASE("The list of a converted line loop is cached with its type", "[graphics][primitive]") {
  Host host(false);
  Indices16 indices{1, 2, 3, 0xFFFF, 7, 8};
  host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop, indices, 0xFFFF);
  // No BeginDraw in between: the second draw of the frame takes the cached
  // conversion.
  PrimitiveProcessor::ProcessingResult result;
  REQUIRE(host.processor.Process(result));
  CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
  CHECK(result.host_draw_vertex_count == 10);
}

TEST_CASE("Draws of one buffer with different reset indices don't share a conversion",
          "[graphics][primitive]") {
  Host host(false);

  SECTION("16-bit") {
    host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                        Indices16{0, 1, 2, 0x1234, 3, 4, 0x5678, 5}, 0x1234);
    auto result = host.DrawAgain(0x5678);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kHostConverted);
    CHECK(host.processor.LastConverted<uint16_t>(8) ==
          Expected{0, 1, 2, 0x1234, 3, 4, 0xFFFF, 5});
  }
  SECTION("32-bit") {
    host.Draw<uint32_t>(xenos::PrimitiveType::kTriangleStrip,
                        Indices32{0, 1, 2, 0x1234, 3, 4, 0x5678, 5}, 0x1234);
    auto result = host.DrawAgain(0x5678);
    CHECK(result.index_buffer_type == PrimitiveProcessor::kHostConverted);
    CHECK(host.processor.LastConverted<uint32_t>(8) ==
          Expected{0, 1, 2, 0x1234, 3, 4, 0xFFFFFFFF, 5});
  }
  SECTION("A converted primitive type") {
    auto first = host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop,
                                     Indices16{1, 2, 3, 0x1234, 7, 8}, 0x1234);
    CHECK(first.host_primitive_type == xenos::PrimitiveType::kLineList);
    auto result = host.DrawAgain(0x7777);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineStrip);
    CHECK(result.host_draw_vertex_count == 7);
  }
  SECTION("The same reset index still takes the cached conversion") {
    host.Draw<uint16_t>(xenos::PrimitiveType::kTriangleStrip,
                        Indices16{0, 1, 2, 0x1234, 3, 4, 5}, 0x1234);
    host.DrawAgain(0x1234);
    CHECK(host.processor.Converted<uint16_t>(7) == Expected{0, 1, 2, 0xFFFF, 3, 4, 5});
  }
}

TEST_CASE("A line loop with an all-ones vertex survives hosts that always cut strips",
          "[graphics][primitive]") {
  Host host(true);

  SECTION("16-bit") {
    auto result =
        host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop, Indices16{1, 0xFFFF, 3}, kNoReset);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
    REQUIRE(result.host_draw_vertex_count == 6);
    CHECK(host.processor.Converted<uint16_t>(6) == Expected{1, 0xFFFF, 0xFFFF, 3, 3, 1});
  }
  SECTION("32-bit") {
    auto result = host.Draw<uint32_t>(xenos::PrimitiveType::kLineLoop,
                                      Indices32{1, 0xFFFFFFFF, 3}, kNoReset);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineList);
    REQUIRE(result.host_draw_vertex_count == 6);
    CHECK(host.processor.Converted<uint32_t>(6) == Expected{1, 0xFFFFFFFF, 0xFFFFFFFF, 3, 3, 1});
  }
  SECTION("Without such a vertex it stays a strip") {
    auto result =
        host.Draw<uint16_t>(xenos::PrimitiveType::kLineLoop, Indices16{1, 2, 3}, kNoReset);
    CHECK(result.host_primitive_type == xenos::PrimitiveType::kLineStrip);
  }
}
