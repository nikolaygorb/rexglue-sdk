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

#pragma once

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

namespace rex::audio::nop {

// Accepts frames and discards them. The audio worker paces the guest's
// callbacks itself and uses the semaphore only as back-pressure, so a frame
// is released as soon as it is submitted.
class NopAudioDriver : public AudioDriver {
 public:
  NopAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~NopAudioDriver() override;

  void SubmitFrame(uint32_t samples_ptr) override;

 private:
  rex::thread::Semaphore* semaphore_ = nullptr;
};

}  // namespace rex::audio::nop
