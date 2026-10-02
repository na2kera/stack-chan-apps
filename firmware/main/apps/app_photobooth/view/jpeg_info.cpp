/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "jpeg_info.h"

namespace photobooth::view {

bool readJpegInfo(const uint8_t* data, size_t len, JpegInfo& out)
{
    out = JpegInfo{};
    if (data == nullptr || len < 4 || data[0] != 0xFF || data[1] != 0xD8) {  // SOI
        return false;
    }
    size_t pos = 2;
    for (;;) {
        // マーカー: 0xFF (詰め物の 0xFF が続いてもよい) + 種類 1 バイト
        if (pos >= len || data[pos] != 0xFF) {
            return false;
        }
        while (pos < len && data[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= len) {
            return false;
        }
        const uint8_t marker = data[pos++];
        if (marker == 0x00) {
            return false;  // ヘッダの中に 0xFF00 (スキャンデータの詰め物) は現れない
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;  // TEM / RSTn: 長さを持たない
        }
        if (marker == 0xD8 || marker == 0xD9 || marker == 0xDA) {
            return false;  // SOI の重複、EOI、または SOF より前の SOS
        }
        // 長さ (2 バイト、長さ自身を含む) を持つセグメント
        if (len - pos < 2) {
            return false;
        }
        const size_t seg = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
        if (seg < 2 || seg > len - pos) {
            return false;
        }
        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
            // SOF: 長さ(2) 精度(1) 高さ(2) 幅(2) 成分数(1) ...
            if (seg < 8) {
                return false;
            }
            out.height      = (static_cast<int>(data[pos + 3]) << 8) | data[pos + 4];
            out.width       = (static_cast<int>(data[pos + 5]) << 8) | data[pos + 6];
            out.progressive = marker == 0xC2;
            return out.width > 0 && out.height > 0;
        }
        pos += seg;
    }
}

}  // namespace photobooth::view
