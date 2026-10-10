/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "clips.h"

#include "../assets/pb_assets.h"

namespace photobooth::hw {

using shared::hw::Audio;

void Clips::begin()
{
    pcm_[static_cast<int>(Clip::Announce)] = Audio::parseWav(pb_voice_announce_start, pb_voice_announce_end, "announce");
    pcm_[static_cast<int>(Clip::Captured)] = Audio::parseWav(pb_voice_captured_start, pb_voice_captured_end, "captured");
    pcm_[static_cast<int>(Clip::Closer)]   = Audio::parseWav(pb_voice_closer_start, pb_voice_closer_end, "closer");
    pcm_[static_cast<int>(Clip::Shutter)]  = Audio::parseWav(pb_voice_shutter_start, pb_voice_shutter_end, "shutter");
}

}  // namespace photobooth::hw
