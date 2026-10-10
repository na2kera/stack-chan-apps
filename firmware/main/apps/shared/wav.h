/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 埋め込み WAV (RIFF/WAVE) のヘッダを読んで PCM の位置を返す。
// HAL・FreeRTOS・ログに依存しない純粋な関数 (firmware/tests/wav_test.cpp でホストでテストする)。
// ログは呼び出し側 (shared/hw/audio.cpp の Audio::parseWav) が Status を見て出す。
#pragma once

#include <cstddef>
#include <cstdint>

namespace shared::wav {

struct Format {
    uint16_t format   = 0;  // 1 = PCM
    uint16_t channels = 0;
    uint32_t rate     = 0;
    uint16_t bits     = 0;
};

struct Pcm {
    const uint8_t* data = nullptr;  // 16-bit LE mono のサンプル列 (入力のバッファを指す)
    size_t samples      = 0;
    Format format;  // fmt チャンクの内容 (Unsupported のときのログ用。NotRiff / NotFound では空)
};

enum class Status : uint8_t {
    Ok,
    NotRiff,      // RIFF/WAVE のヘッダでない (短すぎる場合を含む)
    Unsupported,  // fmt チャンクはあるが、PCM / mono / 16-bit / 指定のレートでない
    NotFound,     // fmt チャンクより前に data チャンクが来た、または fmt / data チャンクが無い (途中で切れている場合を含む)
};

// data..data+len の WAV を調べ、PCM mono 16-bit で rate Hz のサンプル列なら out に位置を入れて Ok。
// チャンクはバッファに収まるものだけ読む (サイズがバッファを超えるチャンクでやめる)。
Status parse(const uint8_t* data, size_t len, uint32_t rate, Pcm& out);

}  // namespace shared::wav
