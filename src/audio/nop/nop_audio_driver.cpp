/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    2026 - Ported for the ReXGlue fork
 */

#include <rex/audio/nop/nop_audio_driver.h>

namespace rex::audio::nop {

NopAudioDriver::NopAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

NopAudioDriver::~NopAudioDriver() = default;

void NopAudioDriver::SubmitFrame(uint32_t /*samples_ptr*/) {
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

}  // namespace rex::audio::nop
