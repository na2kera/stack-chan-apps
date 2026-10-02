/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "jpeg_info.h"

namespace photobooth::view {

JpegStatus readJpegInfo(const uint8_t* data, size_t len, JpegInfo& out)
{
    out = JpegInfo{};
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {  // SOI
        return JpegStatus::Invalid;
    }
    size_t pos = 2;
    for (;;) {
        // マーカー: 0xFF (詰め物の 0xFF が続いてもよい) + 種類 1 バイト
        if (pos >= len || data[pos] != 0xFF) {
            return JpegStatus::Invalid;
        }
        while (pos < len && data[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= len) {
            return JpegStatus::Invalid;
        }
        const uint8_t marker = data[pos++];
        if (marker == 0x00) {
            return JpegStatus::Invalid;  // ヘッダの中に 0xFF00 (スキャンデータの詰め物) は現れない
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // TEM / RSTn: 長さを持たない
        }
        if (marker == 0xD8 || marker == 0xD9 || marker == 0xDA) {
            return JpegStatus::Invalid;  // SOI の重複、EOI、または SOF より前の SOS
        }
        // 長さ (2 バイト、長さ自身を含む) を持つセグメント
        if (len - pos < 2) {
            return JpegStatus::Invalid;
        }
        const size_t seg = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        if (seg < 2 || seg > len - pos) {
            return JpegStatus::Invalid;
        }
        // SOF0..SOF15 (0xC0..0xCF)。ただし 0xC4 = DHT、0xC8 = JPG (予約)、0xCC = DAC は SOF ではない
        const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            // SOF: 長さ(2) 精度(1) 高さ(2) 幅(2) 成分数(1) ...
            if (seg < 8) {
                return JpegStatus::Invalid;
            }
            out.sof        = marker;
            out.precision  = data[pos + 2];
            out.height     = (static_cast<int>(data[pos + 3]) << 8) | data[pos + 4];
            out.width      = (static_cast<int>(data[pos + 5]) << 8) | data[pos + 6];
            out.components = data[pos + 7];
            if (out.width <= 0 || out.height <= 0) {
                return JpegStatus::Invalid;
            }
            // SOF0 の長さは 8 + 成分ごとの 3 バイト (ID・サンプリング係数・量子化表) とちょうど一致する
            // (セグメント全体がバッファに収まることは上で確認済み)。合わなければ壊れている。
            if (marker == 0xC0 && seg != 8 + 3 * static_cast<size_t>(out.components)) {
                return JpegStatus::Invalid;
            }
            // 純正のデコーダで出せるのはベースラインだけ。
            const bool baseline = marker == 0xC0 && out.precision == 8 && (out.components == 1 || out.components == 3);
            return baseline ? JpegStatus::Ok : JpegStatus::Unsupported;
        }
        pos += seg;
    }
}

}  // namespace photobooth::view
