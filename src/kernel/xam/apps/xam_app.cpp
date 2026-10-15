/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <string>

#include <rex/kernel/xam/apps/app.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XamApp::XamApp(KernelState* kernel_state) : App(kernel_state, 0xFE) {}

X_HRESULT XamApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0002000E: {
      struct message_data {
        rex::be<uint32_t> user_index;
        rex::be<uint32_t> unk_04;
        rex::be<uint32_t> extra_ptr;
        rex::be<uint32_t> buffer_ptr;
        rex::be<uint32_t> buffer_size;
        rex::be<uint32_t> unk_14;
        rex::be<uint32_t> length_ptr;
        rex::be<uint32_t> unk_1C;
      }* data = reinterpret_cast<message_data*>(buffer);
      REXKRNL_DEBUG(
          "XamAppEnumerateContentAggregate({}, {:08X}, {:08X}, {:08X}, {}, "
          "{:08X}, {:08X}, {:08X})",
          (uint32_t)data->user_index, (uint32_t)data->unk_04, (uint32_t)data->extra_ptr,
          (uint32_t)data->buffer_ptr, (uint32_t)data->buffer_size, (uint32_t)data->unk_14,
          (uint32_t)data->length_ptr, (uint32_t)data->unk_1C);
      auto extra = memory_->TranslateVirtual<X_KENUMERATOR_CONTENT_AGGREGATE*>(data->extra_ptr);
      auto buffer = memory_->TranslateVirtual(data->buffer_ptr);
      auto e = kernel_state_->object_table()->LookupObject<XEnumerator>(extra->handle);
      if (!e || !buffer || !extra) {
        return X_E_INVALIDARG;
      }
      assert_true(extra->magic == kXObjSignature);
      if (data->buffer_size) {
        std::memset(buffer, 0, data->buffer_size);
      }
      uint32_t item_count = 0;
      auto result = e->WriteItems(data->buffer_ptr, buffer, &item_count);

      // The caller reads the count whatever the result, so always write it.
      if (data->length_ptr) {
        auto length_ptr = memory_->TranslateVirtual<be<uint32_t>*>(data->length_ptr);
        *length_ptr = (result == X_ERROR_SUCCESS && item_count >= 1) ? 1 : 0;
      }
      if (result == X_ERROR_SUCCESS && item_count >= 1) {
        return X_E_SUCCESS;
      }
      return X_E_NO_MORE_FILES;
    }
    case 0x00020021: {
      // XContentQueryVolumeDeviceType, as xenia-canary answers it.
      struct XContentQueryVolumeDeviceType {
        char root_name[64];
        rex::be<uint32_t> is_title_process;
        rex::be<uint32_t> device_type_ptr;
        rex::be<uint32_t> overlapped_ptr;
      }* data = reinterpret_cast<XContentQueryVolumeDeviceType*>(buffer);
      assert_true(buffer_length == sizeof(XContentQueryVolumeDeviceType));
      REXKRNL_DEBUG("XContentQueryVolumeDeviceType('{}', {:08X}, {:08X}, {:08X})", data->root_name,
                    (uint32_t)data->is_title_process, (uint32_t)data->device_type_ptr,
                    (uint32_t)data->overlapped_ptr);

      // Only a root that links to an XContent package has a volume device
      // type; a title running from disc (an extracted image) gets an error.
      std::string target;
      const std::string root(data->root_name, strnlen(data->root_name, sizeof(data->root_name)));
      if (!REX_KERNEL_FS()->FindSymbolicLink(root + ':', target) ||
          !target.starts_with("\\Device\\Package_")) {
        return X_E_INVALIDARG;
      }
      // Packages live on the (emulated) hard drive.
      if (data->device_type_ptr) {
        *memory_->TranslateVirtual<rex::be<uint32_t>*>(data->device_type_ptr) =
            static_cast<uint32_t>(DeviceType::HDD);
      }
      return X_E_SUCCESS;
    }
    case 0x00021012: {
      REXKRNL_DEBUG("XamApp(0x00021012)");
      return X_E_SUCCESS;
    }
    case 0x00022005: {
      struct message_data {
        rex::be<uint32_t> unk_00;  // ? output_ptr ?
        rex::be<uint32_t> unk_04;  // ? value/jump to? ?
      }* data = reinterpret_cast<message_data*>(buffer);
      assert_true(buffer_length == sizeof(message_data));
      auto unk = memory_->TranslateVirtual<rex::be<uint32_t>*>(data->unk_00);
      auto adr = *unk;
      REXKRNL_DEBUG("XamApp(0x00022005)(%.8X, %.8X)", (uint32_t)data->unk_00,
                    (uint32_t)data->unk_04);
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XAM message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
