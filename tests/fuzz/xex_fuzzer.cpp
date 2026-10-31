/**
 * @file        fuzz/xex_fuzzer.cpp
 * @brief       libFuzzer target for loading XEX files (headers, image
 *              decompression, PE headers)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include <rex/system/function_dispatcher.h>
#include <rex/system/util/xex2_info.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

#include "last_input.h"

using namespace rex;

// Input layout: a XEX file. tools/fuzz/make_xex_corpus.py writes small ones.
//
// XexModule::Load and LoadContinue on real guest memory, then Unload. The
// import libraries are not read: resolving them needs a running kernel, so the
// header's key is changed before loading.

namespace {

constexpr uint32_t kImportLibrariesKeyDisabled = 0xFFFFFFFF;

struct Guest {
  memory::Memory memory;
  std::unique_ptr<runtime::FunctionDispatcher> dispatcher;
};

Guest* GetGuest() {
  static Guest* guest = [] {
    auto* guest = new Guest;
    if (!guest->memory.Initialize()) {
      std::fprintf(stderr, "xex_fuzzer: can't reserve the guest address space\n");
      std::abort();
    }
    guest->dispatcher = std::make_unique<runtime::FunctionDispatcher>(&guest->memory, nullptr);
    return guest;
  }();
  return guest;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  rex::fuzz::SaveLastInput(data, size);
  if (size < sizeof(xex2_header)) {
    return 0;
  }
  // An exact-size copy, which also takes the change of the key.
  std::vector<uint8_t> file(data, data + size);
  auto* header = reinterpret_cast<xex2_header*>(file.data());
  size_t header_count =
      std::min<size_t>(header->header_count,
                       (size - offsetof(xex2_header, headers)) / sizeof(xex2_opt_header));
  for (size_t i = 0; i < header_count; ++i) {
    if (header->headers[i].key == XEX_HEADER_IMPORT_LIBRARIES) {
      header->headers[i].key = xex2_header_keys(kImportLibrariesKeyDisabled);
    }
  }

  Guest* guest = GetGuest();
  runtime::XexModule module(guest->dispatcher.get(), nullptr);
  if (module.Load("fuzz", "fuzz", file.data(), file.size())) {
    if (!module.is_patch()) {
      module.LoadContinue();
    }
    module.Unload();
  }
  return 0;
}
