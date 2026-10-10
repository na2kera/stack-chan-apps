/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "frame_jpeg.h"

#include <jpg/image_to_jpeg.h>  // 純正 (xiaozhi-esp32) の JPEG エンコーダ。esp_new_jpeg (ESP32-S3 はソフトウェア)

#include <cstdlib>

namespace photobooth::net {

// 入力形式: V4L2_PIX_FMT_RGB565 は image_to_jpeg() の中で ESP_IMGFX_PIXEL_FMT_RGB565_LE として YUYV に変換される
// (image_to_jpeg.cpp convert_input_to_encoder_buf)。カメラ層 (hw/camera.h) の FrameView も RGB565 LE なので、
// バイト順の変換は要らない。
// 確保先: YUYV の作業領域 (150 KiB) は jpeg_calloc_align (PSRAM 優先、無ければ内部 RAM)、出力 (約 177 KiB) は
// malloc (CONFIG_SPIRAM_USE_MALLOC=y、ALWAYSINTERNAL=512 なので PSRAM 優先)、esp_new_jpeg のワーク領域
// (約 10 KiB) は内部 RAM 優先。
bool encodeRgb565Jpeg(uint8_t* rgb565_le, size_t len, uint16_t width, uint16_t height, uint8_t quality, uint8_t** out,
                      size_t* out_len)
{
    *out     = nullptr;
    *out_len = 0;
    if (!image_to_jpeg(rgb565_le, len, width, height, V4L2_PIX_FMT_RGB565, quality, out, out_len)) {
        *out     = nullptr;  // 失敗時は出力を返さない (image_to_jpeg は解放済み)
        *out_len = 0;
        return false;
    }
    if (*out == nullptr || *out_len == 0) {
        free(*out);
        *out     = nullptr;
        *out_len = 0;
        return false;
    }
    return true;
}

}  // namespace photobooth::net
