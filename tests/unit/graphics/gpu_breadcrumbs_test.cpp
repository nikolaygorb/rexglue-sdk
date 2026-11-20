#include <catch2/catch_test_macros.hpp>
#include <rex/ui/gpu_breadcrumbs.h>

#include <string>

TEST_CASE("GPU breadcrumb trail retains work identity and rejects overwritten entries",
          "[graphics]") {
  rex::ui::GpuBreadcrumbTrail trail;
  CHECK_FALSE(trail.Find(0));
  const uint32_t first = trail.Record({0, 10, 20, 0x1234, 0x5678, 30, false});
  auto entry = trail.Find(first);
  REQUIRE(entry);
  CHECK(entry->submission == 10);
  CHECK(entry->frame == 20);
  CHECK(entry->vertex_shader == 0x1234);
  CHECK(entry->pixel_shader == 0x5678);
  CHECK(entry->vertices == 30);
  for (size_t i = 0; i < rex::ui::GpuBreadcrumbTrail::kCapacity; ++i) {
    trail.Record({0, 11, 21, 0, 0, 0, true});
  }
  CHECK_FALSE(trail.Find(first));
  REQUIRE(trail.Find(first + rex::ui::GpuBreadcrumbTrail::kCapacity));
  CHECK(trail.Find(first + rex::ui::GpuBreadcrumbTrail::kCapacity)->resolve);
}

TEST_CASE("GPU breadcrumb details tell the work before a draw from the draw", "[graphics]") {
  using namespace rex::ui;
  GpuBreadcrumbTrail trail;
  GpuBreadcrumbDetails first;
  first.filled = true;
  first.render_pass_kind = GpuRenderPassKind::kXenosVulkan;
  // 1x, depth used (D24S8), color 0 used as 2_10_10_10_FLOAT (3).
  first.render_pass = 0x00000300u | (0b00011u << 2);
  const uint32_t finished = trail.Record({0, 1, 1, 0xA, 0xB, 3, false, first});
  CHECK(trail.Next() == finished + 1);
  CHECK(DescribeGpuBreadcrumbDetails(trail, finished, 0).empty());  // nothing unfinished

  GpuBreadcrumbDetails second = first;
  second.vertex_shader_modification = 0x12;
  second.pixel_shader_modification = 0x34;
  second.work.render_passes = 1;
  second.work.draws = 3;
  second.work.barriers = 2;
  second.setup_marked = true;
  const uint32_t hung = trail.Record({0, 1, 1, 0xC, 0xD, 6, false, second});
  const std::string in_work = DescribeGpuBreadcrumbDetails(trail, finished, 0);
  CHECK(in_work.find("unfinished #" + std::to_string(hung) + " (draw)") != std::string::npos);
  CHECK(in_work.find("VS modification 0000000000000012") != std::string::npos);
  CHECK(in_work.find("1 render pass, 3 draws, 2 barriers") != std::string::npos);
  CHECK(in_work.find("depth D24S8, color 0 2_10_10_10_FLOAT") != std::string::npos);
  CHECK(in_work.find("did not finish that work") != std::string::npos);
  const std::string in_draw = DescribeGpuBreadcrumbDetails(trail, finished, hung);
  CHECK(in_draw.find("it hung in the draw itself") != std::string::npos);
}

TEST_CASE("GPU breadcrumb details without a second marker", "[graphics]") {
  using namespace rex::ui;
  GpuBreadcrumbTrail trail;
  GpuBreadcrumbDetails draw;
  draw.filled = true;
  const uint32_t color[4] = {44, 0, 0, 0};  // VK_FORMAT_B8G8R8A8_UNORM
  draw.render_pass = PackNativeRenderPass(4, 129, color);  // D24_UNORM_S8_UINT
  draw.render_pass_kind = GpuRenderPassKind::kNative;
  const uint32_t finished = trail.Record({0, 1, 1, 1, 1, 3, false, draw});
  trail.Record({0, 1, 1, 1, 1, 3, false, draw});  // no work of its own
  const std::string plain = DescribeGpuBreadcrumbDetails(trail, finished, 0);
  CHECK(plain.find("render pass (4x, depth VkFormat 129, color 0 VkFormat 44)") !=
        std::string::npos);
  CHECK(plain.find("nothing else was recorded after #" + std::to_string(finished)) !=
        std::string::npos);

  GpuBreadcrumbDetails with_work = draw;
  with_work.work.dispatches = 1;
  const uint32_t before = trail.Last();
  trail.Record({0, 1, 1, 1, 1, 3, false, with_work});
  CHECK(DescribeGpuBreadcrumbDetails(trail, before, 0).find(
            "gpu_device_lost_setup_breadcrumbs tells them apart") != std::string::npos);

  GpuBreadcrumbDetails resolve;
  resolve.filled = true;
  resolve.work.copies = 2;
  const uint32_t before_resolve = trail.Last();
  trail.Record({0, 1, 1, 0, 0, 0, true, resolve});
  CHECK(DescribeGpuBreadcrumbDetails(trail, before_resolve, 0).find(
            "(resolve): recorded with it: 2 copies") != std::string::npos);
  CHECK(DescribeGpuBreadcrumbWork(GpuBreadcrumbWork()) == "nothing");
}
