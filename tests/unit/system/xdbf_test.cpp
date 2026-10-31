/**
 * @file        system/xdbf_test.cpp
 * @brief       Tests for reading a damaged XDBF (SPA) resource
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/system/util/xdbf_utils.h>
#include <rex/system/xcontent.h>

namespace {

using namespace rex::system;
using namespace rex::system::util;

constexpr size_t kHeaderSize = 24;
constexpr size_t kEntrySize = 18;
constexpr size_t kFreeSize = 8;
constexpr uint64_t kIdXstc = 0x58535443;
constexpr uint64_t kIdXach = 0x58414348;
constexpr uint16_t kTitleStringId = 0x8000;

void Append(std::vector<uint8_t>& out, uint64_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void AppendFourcc(std::vector<uint8_t>& out, const char* fourcc) {
  out.insert(out.end(), fourcc, fourcc + 4);
}

struct Entry {
  XdbfSection section;
  uint64_t id;
  std::vector<uint8_t> content;
};

// A resource with one free table entry, its blocks one after another.
std::vector<uint8_t> MakeXdbf(const std::vector<Entry>& entries) {
  std::vector<uint8_t> out;
  AppendFourcc(out, "XDBF");
  Append(out, 1, 4);
  Append(out, entries.size(), 4);
  Append(out, entries.size(), 4);
  Append(out, 1, 4);
  Append(out, 0, 4);
  uint32_t offset = 0;
  for (const Entry& entry : entries) {
    Append(out, uint16_t(entry.section), 2);
    Append(out, entry.id, 8);
    Append(out, offset, 4);
    Append(out, entry.content.size(), 4);
    offset += uint32_t(entry.content.size());
  }
  Append(out, 0, kFreeSize);
  for (const Entry& entry : entries) {
    out.insert(out.end(), entry.content.begin(), entry.content.end());
  }
  return out;
}

std::vector<uint8_t> MakeSection(const char* fourcc, uint32_t count) {
  std::vector<uint8_t> out;
  AppendFourcc(out, fourcc);
  Append(out, 1, 4);
  Append(out, 0, 4);
  Append(out, count, 2);
  return out;
}

std::vector<uint8_t> MakeStrings(uint16_t id, const std::string& text) {
  std::vector<uint8_t> out = MakeSection("XSTR", 1);
  Append(out, id, 2);
  Append(out, text.size(), 2);
  out.insert(out.end(), text.begin(), text.end());
  return out;
}

Entry EnglishStrings(std::vector<uint8_t> content) {
  return {XdbfSection::kStringTable, uint64_t(XLanguage::kEnglish), std::move(content)};
}

std::vector<uint8_t> MakeAchievements(uint32_t claimed, uint32_t stored) {
  std::vector<uint8_t> out = MakeSection("XACH", claimed);
  for (uint32_t i = 0; i < stored; ++i) {
    Append(out, i + 1, 2);
    out.resize(out.size() + sizeof(XdbfAchievementTableEntry) - 2);
  }
  return out;
}

// The position of a field of entry 0 in the resource.
constexpr size_t kEntry0Offset = kHeaderSize + 10;
constexpr size_t kEntry0Size = kHeaderSize + 14;

void Patch32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    data[offset + i] = uint8_t(value >> ((3 - i) * 8));
  }
}

}  // namespace

TEST_CASE("A well-formed XDBF resource reads", "[system][xdbf]") {
  std::vector<uint8_t> data =
      MakeXdbf({EnglishStrings(MakeStrings(kTitleStringId, "Title")),
                {XdbfSection::kMetadata, kIdXach, MakeAchievements(2, 2)}});
  XdbfGameData db(data.data(), data.size());
  REQUIRE(db.is_valid());
  CHECK(db.title() == "Title");
  auto achievements = db.GetAchievements();
  REQUIRE(achievements.size() == 2);
  CHECK(uint16_t(achievements[1].id) == 2);
}

TEST_CASE("An XDBF table past the end of the resource is invalid", "[system][xdbf]") {
  std::vector<uint8_t> data = MakeXdbf({EnglishStrings(MakeStrings(kTitleStringId, "Title"))});

  SECTION("Entries") {
    Patch32(data, 8, 0x10000000);
    Patch32(data, 12, 1);
  }
  SECTION("Free list") { Patch32(data, 16, 0xFFFFFFFF); }
  SECTION("More entries used than the table holds") { Patch32(data, 12, 2); }

  XdbfGameData db(data.data(), data.size());
  CHECK_FALSE(db.is_valid());
  CHECK_FALSE(db.GetEntry(XdbfSection::kStringTable, uint64_t(XLanguage::kEnglish)));
}

TEST_CASE("A truncated XDBF header table is invalid", "[system][xdbf]") {
  std::vector<uint8_t> data = MakeXdbf({EnglishStrings(MakeStrings(kTitleStringId, "Title"))});
  XdbfGameData db(data.data(), kHeaderSize + kEntrySize + kFreeSize - 1);
  CHECK_FALSE(db.is_valid());
}

TEST_CASE("An XDBF block outside the resource is not returned", "[system][xdbf]") {
  std::vector<uint8_t> data = MakeXdbf({EnglishStrings(MakeStrings(kTitleStringId, "Title"))});
  uint32_t content_size = uint32_t(data.size() - (kHeaderSize + kEntrySize + kFreeSize));

  SECTION("Offset") { Patch32(data, kEntry0Offset, content_size); }
  SECTION("Offset wrapping 32 bits with the size") { Patch32(data, kEntry0Offset, 0xFFFFFFFF); }
  SECTION("Size") { Patch32(data, kEntry0Size, content_size + 1); }

  XdbfGameData db(data.data(), data.size());
  REQUIRE(db.is_valid());
  CHECK_FALSE(db.GetEntry(XdbfSection::kStringTable, uint64_t(XLanguage::kEnglish)));
  CHECK(db.title().empty());
}

TEST_CASE("A block ending at the end of the resource is returned", "[system][xdbf]") {
  std::vector<uint8_t> data = MakeXdbf({{XdbfSection::kImage, 7, {1, 2, 3}}});
  XdbfGameData db(data.data(), data.size());
  XdbfBlock block = db.GetEntry(XdbfSection::kImage, 7);
  REQUIRE(block);
  CHECK(block.size == 3);
  CHECK(block.buffer + block.size == data.data() + data.size());
}

TEST_CASE("XDBF string tables are read only inside their block", "[system][xdbf]") {
  std::vector<uint8_t> content;

  SECTION("The block is smaller than the section header") {
    content = {'X', 'S', 'T', 'R'};
  }
  SECTION("More strings claimed than the block holds") {
    content = MakeSection("XSTR", 2);
  }
  SECTION("A string entry cut in the middle") {
    content = MakeSection("XSTR", 1);
    Append(content, kTitleStringId, 2);
  }
  SECTION("A string longer than the block") {
    content = MakeStrings(kTitleStringId, "Title");
    content[sizeof(XdbfSectionHeader) + 3] = 6;
  }
  SECTION("A string skipped on the way that is longer than the block") {
    content = MakeStrings(1, "Text");
    content[sizeof(XdbfSectionHeader) + 2] = 0xFF;
  }

  // Another block behind this one, which must not be read as its string data.
  std::vector<uint8_t> data =
      MakeXdbf({EnglishStrings(content), {XdbfSection::kImage, 7, std::vector<uint8_t>(0x10000)}});
  XdbfGameData db(data.data(), data.size());
  REQUIRE(db.is_valid());
  CHECK(db.title().empty());
}

TEST_CASE("Only the achievements the XDBF block holds are read", "[system][xdbf]") {
  SECTION("Claimed more than stored") {
    std::vector<uint8_t> data =
        MakeXdbf({{XdbfSection::kMetadata, kIdXach, MakeAchievements(0xFFFF, 2)}});
    XdbfGameData db(data.data(), data.size());
    CHECK(db.GetAchievements().size() == 2);
  }
  SECTION("The block is smaller than the section header") {
    std::vector<uint8_t> data = MakeXdbf({{XdbfSection::kMetadata, kIdXach, {'X', 'A'}}});
    XdbfGameData db(data.data(), data.size());
    CHECK(db.GetAchievements().empty());
  }
}

TEST_CASE("An XDBF language block too small to read is the default", "[system][xdbf]") {
  std::vector<uint8_t> data = MakeXdbf({{XdbfSection::kMetadata, kIdXstc, {'X', 'S', 'T', 'C'}}});
  XdbfGameData db(data.data(), data.size());
  CHECK(db.default_language() == XLanguage::kEnglish);
}
