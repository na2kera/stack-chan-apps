/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// セリフの再生 (docs/design/step1-device.md §5 audio を純正のコーデック経路に移植)。
//
// 埋め込み WAV (assets/voice/*.wav, 24 kHz / mono / 16-bit) の PCM を、専用の FreeRTOS タスクから
// 20 ms ずつ Board::GetInstance().GetAudioCodec()->OutputData() に書く。
// 純正の main/hal/audio.cpp (マイクテスト) と同じ使い方。begin() で EnableOutput(true)、end() で false。
// マイクは使わない (AI エージェントがコーデック入力を持つのはそのアプリの間だけなので競合しない)。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace photobooth::hw {

struct AudioWorker;

class Audio {
public:
    enum class Clip : uint8_t {
        Announce,  // 「写真を撮るよ！ いい顔をしてね」
        Captured,  // 「撮れたよ」(シャッター音代わり)
        Closer,    // 「もう少し寄ってね」(顔判定が入るステップ2以降で使う)
        Shutter,   // 合成のシャッター音 (カシャッ、240 ms。自動採用の直後に captured.wav の前に鳴らす)
        Count,     // 個数 (clips_ の大きさ)
    };

    // PCM の位置 (埋め込みデータを指すだけ)。
    struct Pcm {
        const uint8_t* data = nullptr;  // 16-bit LE mono
        size_t samples      = 0;
        const char* name    = "";
    };

    // 出力を有効にして再生タスクを作る。コーデックが無い、または前回止めきれなかった再生タスクが
    // まだ生きているときは false (以後 play() は何もしない)。
    bool begin();
    // 再生を止めてタスクを終わらせ、出力を無効にする。タスクが 1 秒以内に止まらなければ切り離し、
    // 出力の無効化はタスクが終わるときに任せる (OutputData の途中で出力を閉じないため)。
    void end();

    // 再生を始める (非同期)。再生中なら差し替える。WAV が不正なら false。
    bool play(Clip clip);
    bool isPlaying() const;
    void stop();

private:
    static Pcm parseWav(const uint8_t* start, const uint8_t* end, const char* name);

    Pcm clips_[static_cast<int>(Clip::Count)];
    // タスクと共有する状態。タスクも shared_ptr を持つので、Audio が先に消えても安全。
    std::shared_ptr<AudioWorker> worker_;
};

}  // namespace photobooth::hw
