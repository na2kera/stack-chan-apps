/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// JPEG のヘッダから画像の大きさだけを読む (デコードも確保もしない)。
// edge から受けた候補 JPEG を、デコード用の領域を確保する前に確かめるために使う。
// LVGL・HAL に依存しない純粋な関数。
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::view {

struct JpegInfo {
    int width       = 0;
    int height      = 0;
    bool progressive = false;  // SOF2 (プログレッシブ)
};

// SOI から SOF0 / SOF1 / SOF2 までセグメントをたどって幅・高さを読む。
// 途中でデータが足りない・長さが不正・SOF の前にスキャン (SOS) や EOI が来たら false。
// すべての読み出しで len を超えないことを確かめる。
bool readJpegInfo(const uint8_t* data, size_t len, JpegInfo& out);

}  // namespace photobooth::view
