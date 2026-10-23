/**
 * @file        image_info.h
 * @brief       Image layout descriptor for recompiled binaries
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/types.h>

struct PPCFuncMapping;

namespace rex::system {
class KernelState;
}

namespace rex {

/**
 * Callback for registering recompiled modules with KernelState (multi-binary projects).
 */
using RegisterModulesFunc = void (*)(system::KernelState*);

struct PPCCodegenFlags {
  bool skip_lr = false;
  bool ctr_as_local = false;
  bool xer_as_local = false;
  bool reserved_as_local = false;
  bool skip_msr = false;
  bool cr_as_local = false;
  bool non_argument_as_local = false;
  bool non_volatile_as_local = false;
};

/// The XEX the code was recompiled from, recorded by codegen. All zero in code
/// generated before it was recorded: the startup check is then skipped.
struct PPCImageIdentity {
  u64 fingerprint = 0;  ///< FNV-1a 64 of the XEX page descriptor table
  u32 page_count = 0;   ///< Entries in that table
  u32 title_id = 0;     ///< Shown in the log and the warning only
  u32 media_id = 0;     ///< Shown in the log and the warning only
};

/// PPC image layout passed from the generated config header into ReXApp.
struct PPCImageInfo {
  u32 code_base;
  u32 code_size;
  u32 image_base;
  u32 image_size;
  const PPCFuncMapping* func_mappings;
  bool rexcrt_heap = false;  ///< Set by codegen when [rexcrt] has heap functions
  RegisterModulesFunc register_modules = nullptr;  ///< Set by codegen for multi-binary projects
  PPCCodegenFlags codegen_flags{};                 ///< Set by codegen from the config flags
  PPCImageIdentity image_identity{};               ///< Set by codegen from the loaded XEX
};

}  // namespace rex
