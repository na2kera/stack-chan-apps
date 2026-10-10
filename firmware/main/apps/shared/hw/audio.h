/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 自作アプリ共通の WAV 再生 (docs/design/step1-device.md §5 audio を純正のコーデック経路に移植。
// app_photobooth から切り出した。docs/design/app-roulette.md §3)。
//
// 埋め込み WAV (24 kHz / mono / 16-bit) の PCM を、専用の FreeRTOS タスクから
// 20 ms ずつ Board::GetInstance().GetAudioCodec()->OutputData() に書く。
// 純正の main/hal/audio.cpp (マイクテスト) と同じ使い方。begin() で EnableOutput(true)、end() で false。
// マイクは使わない (AI エージェントがコーデック入力を持つのはそのアプリの間だけなので競合しない)。
// どの WAV があるかは知らない。アプリが parseWav() で Pcm にしたものを play() に渡す。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace shared::hw {

struct AudioWorker;

class Audio {
public:
    // PCM の位置 (埋め込みデータを指すだけ)。data が nullptr なら不正 (play() は false を返す)。
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

    // 埋め込み WAV (EMBED_FILES の _start / _end) を調べ、PCM の位置を返す。
    // 24 kHz / mono / 16-bit PCM でなければ data = nullptr (ログを出す)。
    static Pcm parseWav(const uint8_t* start, const uint8_t* end, const char* name);

    // 再生を始める (非同期)。再生中なら差し替える。WAV が不正なら false。
    bool play(const Pcm& pcm);
    bool isPlaying() const;
    void stop();

private:
    // タスクと共有する状態。タスクも shared_ptr を持つので、Audio が先に消えても安全。
    std::shared_ptr<AudioWorker> worker_;
};

}  // namespace shared::hw
