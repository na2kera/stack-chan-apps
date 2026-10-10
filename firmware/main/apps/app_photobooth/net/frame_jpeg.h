/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// frame の JPEG 符号化 (docs/design/step6-cloud-device.md §3.3)。ハードとの接点: 純正の image_to_jpeg() を呼ぶだけ。
//
// image_to_jpeg.h は linux/videodev2.h を含み、その _IO / _IOR / _IOW が lwIP の sockets.h と衝突する
// (再定義の警告)。esp_http_client を使う http_edge_client.cpp から切り離すため、別の翻訳単位にしている。
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::net {

// RGB565 LE (hw::FrameView と同じ並び) の 1 枚を JPEG にする。成功すれば *out に image_to_jpeg() が malloc で
// 確保した出力バッファ (QVGA で常に 約 177 KiB を確保) が入り、呼び出し側が送信後に free() する。
// 失敗 (確保失敗を含む) なら false で、*out は nullptr。
// 同時に 1 つだけ呼ぶこと (net タスクからだけ呼ぶ。PSRAM のピークを 1 組に抑える)。
bool encodeRgb565Jpeg(uint8_t* rgb565_le, size_t len, uint16_t width, uint16_t height, uint8_t quality, uint8_t** out,
                      size_t* out_len);

}  // namespace photobooth::net
