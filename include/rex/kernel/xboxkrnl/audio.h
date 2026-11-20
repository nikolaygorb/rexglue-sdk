#pragma once
/**
 * @file        rex/kernel/xboxkrnl/audio.h
 * @brief       Speaker configuration values reported by XAudioGetSpeakerConfig
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdint>
#include <optional>
#include <string_view>

namespace rex::kernel::xboxkrnl {

// XCONFIG_USER_AUDIO_FLAGS: the low word is the analog mode, the high word the
// digital one. Titles mix 5.1 frames whatever this says; some choose their
// surround panning, matrix encoding or a stereo mix from it.
constexpr uint32_t kAudioFlagDolbyProLogic = 0x00000001;
constexpr uint32_t kAudioFlagAnalogMono = 0x00000002;
constexpr uint32_t kAudioFlagDigitalStereo = 0x00000000;
constexpr uint32_t kAudioFlagDolbyDigital = 0x00010000;

// The value for the audio_speaker_config setting: "dolby_digital" (the
// console default, digital 5.1 with Pro Logic on the analog output),
// "pro_logic" (analog surround matrixed into stereo), "stereo" (digital
// stereo), "mono". Anything else gives no value.
inline std::optional<uint32_t> SpeakerConfigFromSetting(std::string_view setting) {
  if (setting == "dolby_digital") {
    return kAudioFlagDolbyDigital | kAudioFlagDolbyProLogic;
  }
  if (setting == "pro_logic") {
    return kAudioFlagDolbyProLogic;
  }
  if (setting == "stereo") {
    return kAudioFlagDigitalStereo;
  }
  if (setting == "mono") {
    return kAudioFlagAnalogMono;
  }
  return std::nullopt;
}

}  // namespace rex::kernel::xboxkrnl
