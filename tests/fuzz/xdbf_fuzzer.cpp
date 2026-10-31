/**
 * @file        fuzz/xdbf_fuzzer.cpp
 * @brief       libFuzzer target for reading the title's XDBF (SPA) resource:
 *              title, languages, achievements and their strings and icons
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

using namespace rex::system;
using namespace rex::system::util;

// Input layout:
//   0     - the user language (user_language cvar), as XLanguage;
//   then  - the XDBF resource, as the XEX section named after the title ID.
// tools/fuzz/make_xdbf_corpus.py writes inputs with a title, two string
// tables, achievements and icons.
//
// The resource is read like at the start of a title
// (KernelState::LoadAchievementsData, XamUserCreateAchievementEnumerator),
// by `rexglue init` (achievements.toml and the icons) and by the achievement
// overlay's icon cache: every block they get is copied out. Checked on top of
// ASan: every block lies inside the resource. In the title, the resource is
// in guest memory, so a block outside it is other guest memory or, past the
// guest address space, a host crash.

namespace {

volatile uint32_t sink;

void Use(const XdbfBlock& block, const uint8_t* data, size_t size) {
  if (!block) {
    return;
  }
  if (block.buffer < data || block.buffer > data + size ||
      block.size > size_t(data + size - block.buffer)) {
    std::fprintf(stderr, "xdbf_fuzzer: a block (offset %td, %zu bytes) is outside the %zu-byte "
                 "resource\n",
                 block.buffer - data, block.size, size);
    std::abort();
  }
  uint32_t sum = 0;
  for (size_t i = 0; i < block.size; ++i) {
    sum += block.buffer[i];
  }
  sink = sum;
}

void Use(const std::string& string) {
  uint32_t sum = 0;
  for (char c : string) {
    sum += uint8_t(c);
  }
  sink = sum;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 1) {
    return 0;
  }
  XLanguage user_language = XLanguage(data[0]);
  // A copy of its own size, for ASan to see the end.
  std::vector<uint8_t> resource(data + 1, data + size);
  const uint8_t* resource_data = resource.empty() ? nullptr : resource.data();

  XdbfGameData db(resource_data, resource.size());
  if (!db.is_valid()) {
    return 0;
  }

  // KernelState::LoadAchievementsData and the achievement enumerator.
  XLanguage language = db.GetExistingLanguage(user_language);
  for (const XdbfAchievementTableEntry& entry : db.GetAchievements()) {
    Use(db.GetStringTableEntry(language, entry.label_id));
    Use(db.GetStringTableEntry(language, entry.description_id));
    Use(db.GetStringTableEntry(language, entry.unachieved_id));
    // The icon cache and `rexglue init`.
    Use(db.GetEntry(XdbfSection::kImage, uint64_t(uint32_t(entry.image_id))), resource_data,
        resource.size());
  }

  // `rexglue init`: the title in the default language and the title icon.
  Use(db.title());
  Use(db.icon(), resource_data, resource.size());
  return 0;
}
