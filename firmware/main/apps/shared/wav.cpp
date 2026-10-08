/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "wav.h"

#include <cstring>

namespace shared::wav {

namespace {

uint32_t le32(const uint8_t* p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

Status parse(const uint8_t* data, size_t len, uint32_t rate, Pcm& out)
{
    out = Pcm{};
    if (data == nullptr || len < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) {
        return Status::NotRiff;
    }
    bool fmt_ok = false;
    size_t pos  = 12;
    while (len - pos >= 8) {
        const uint8_t* chunk = data + pos;
        const uint32_t size  = le32(chunk + 4);
        const uint8_t* body  = chunk + 8;
        if (size > len - pos - 8) {
            break;  // バッファに収まらないチャンク (途中で切れている)
        }
        if (std::memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            out.format.format   = le16(body);
            out.format.channels = le16(body + 2);
            out.format.rate     = le32(body + 4);
            out.format.bits     = le16(body + 14);
            if (out.format.format != 1 || out.format.channels != 1 || out.format.bits != 16 ||
                out.format.rate != rate) {
                return Status::Unsupported;
            }
            fmt_ok = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            if (!fmt_ok) {
                break;
            }
            out.data    = body;
            out.samples = size / 2;
            return Status::Ok;
        }
        // チャンクは 2 バイト境界に詰められる (奇数サイズなら 1 バイトの詰め物)
        pos += 8 + size + (size & 1);
    }
    return Status::NotFound;
}

}  // namespace shared::wav
