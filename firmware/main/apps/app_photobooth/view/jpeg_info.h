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
    int width      = 0;
    int height     = 0;
    int precision  = 0;  // サンプルのビット数
    int components = 0;  // 成分数
    uint8_t sof    = 0;  // 見つかった SOF マーカー (0xC0 = ベースライン、0xC1 = 拡張、0xC2 = プログレッシブ …)
};

enum class JpegStatus : uint8_t {
    Ok,           // ベースライン (SOF0)、8 ビット、1 または 3 成分
    Invalid,      // JPEG として読めない (データ不足・長さ不正・SOF0 の長さが成分数と合わない・SOF の前に SOS / EOI など)
    Unsupported,  // 読めたがベースラインではない (SOF1 / SOF2 など、8 ビット以外、成分数が 1・3 以外)
};

// SOI から最初の SOF までセグメントをたどって、幅・高さ・精度・成分数を読む。
// すべての読み出しで len を超えないことを確かめる。Unsupported のときも out は埋まる (ログ用)。
JpegStatus readJpegInfo(const uint8_t* data, size_t len, JpegInfo& out);

}  // namespace photobooth::view
