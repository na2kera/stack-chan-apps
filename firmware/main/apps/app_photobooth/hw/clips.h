/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth のセリフ・効果音の表 (docs/design/app-roulette.md §3)。
//
// 再生そのものは共通の shared::hw::Audio がする。ここは埋め込み WAV (assets/voice/*.wav) を
// begin() で Audio::parseWav() して持ち、Clip から引けるようにするだけ。
#pragma once

#include <cstdint>

#include "../../shared/hw/audio.h"

namespace photobooth::hw {

enum class Clip : uint8_t {
    Announce,  // 「写真を撮るよ！ いい顔をしてね」
    Captured,  // 「撮れたよ」(シャッター音代わり)
    Closer,    // 「もう少し寄ってね」(顔判定が入るステップ2以降で使う)
    Shutter,   // 合成のシャッター音 (カシャッ、240 ms。自動採用の直後に captured.wav の前に鳴らす)
    Count,     // 個数 (pcm_ の大きさ)
};

class Clips {
public:
    // 4 本の WAV を調べて PCM の位置を覚える (不正な WAV は data = nullptr のまま。play() が false を返す)。
    void begin();

    const shared::hw::Audio::Pcm& get(Clip clip) const
    {
        return pcm_[static_cast<int>(clip)];
    }

private:
    shared::hw::Audio::Pcm pcm_[static_cast<int>(Clip::Count)];
};

}  // namespace photobooth::hw
