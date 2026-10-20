#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <mutex>
#include <span>
#include <string>

#include <rex/net/xnet_startup.h>
#include <rex/ppc/function.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_module.h>
#include <rex/system/kernel_state.h>

namespace rex {
namespace kernel {
namespace xam {

bool xeXamIsUIActive();

class XamModule : public system::KernelModule {
 public:
  XamModule(Runtime* emulator, system::KernelState* kernel_state);
  virtual ~XamModule();

  static void RegisterExportTable(rex::runtime::ExportResolver* export_resolver);

  struct LoaderData {
    bool launch_data_present = false;
    std::vector<uint8_t> launch_data;
    uint32_t launch_flags = 0;
    std::string launch_path;  // Full path to next xex
  };

  const LoaderData& loader_data() const { return loader_data_; }
  LoaderData& loader_data() { return loader_data_; }

  // XNetStartup, or WSAStartup with |wsa| and no |params|. Only the first
  // start since everything was cleaned up takes its parameters.
  void StartXNet(bool wsa, std::span<const uint8_t> params);
  // XNetCleanup, or WSACleanup with |wsa|.
  void CleanupXNet(bool wsa);
  net::XNetStartupParams xnet_startup_params();

 private:
  LoaderData loader_data_;

  std::mutex xnet_mutex_;
  uint32_t xnet_starts_ = 0, wsa_starts_ = 0;
  net::XNetStartupParams xnet_startup_params_ = net::NormalizeXNetStartupParams({});
};

}  // namespace xam
}  // namespace kernel
}  // namespace rex
