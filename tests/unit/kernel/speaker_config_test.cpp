/**
 * @file        kernel/speaker_config_test.cpp
 * @brief       Tests for the audio_speaker_config values (XAudioGetSpeakerConfig)
 *
 * @copyright   Copyright (c) 2026 ReXGlue fork contributors
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <rex/kernel/xboxkrnl/audio.h>

using rex::kernel::xboxkrnl::SpeakerConfigFromSetting;

TEST_CASE("SpeakerConfigFromSetting: console flag values", "[kernel][audio]") {
  // The default keeps what XAudioGetSpeakerConfig always returned (canary's
  // XConfig default: DolbyDigital | DolbyProLogic).
  CHECK(SpeakerConfigFromSetting("dolby_digital") == 0x00010001u);
  CHECK(SpeakerConfigFromSetting("pro_logic") == 0x00000001u);
  CHECK(SpeakerConfigFromSetting("stereo") == 0x00000000u);
  CHECK(SpeakerConfigFromSetting("mono") == 0x00000002u);
}

TEST_CASE("SpeakerConfigFromSetting: unknown names give no value", "[kernel][audio]") {
  CHECK_FALSE(SpeakerConfigFromSetting("").has_value());
  CHECK_FALSE(SpeakerConfigFromSetting("Dolby_Digital").has_value());
  CHECK_FALSE(SpeakerConfigFromSetting("5.1").has_value());
}
