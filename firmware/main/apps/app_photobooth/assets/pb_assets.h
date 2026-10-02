/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の埋め込みリソース。
//   - フォント / アイコン: 同じディレクトリの .c (tools/gen_font.sh, tools/make_icon.py で生成)
//   - セリフ WAV: voice/*.wav を main/CMakeLists.txt の EMBED_FILES で埋め込む
//     (シンボル名は ESP-IDF の規則で _binary_<ファイル名>_start / _end)
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>

LV_FONT_DECLARE(pb_font_jp_20);   // 本文・タイトル・ボタン (ASCII + strings.h の文字)
LV_FONT_DECLARE(pb_font_num_48);  // CAPTURE の残り秒数 (数字と '-')
LV_IMAGE_DECLARE(icon_photobooth);

extern const uint8_t pb_voice_announce_start[] asm("_binary_announce_wav_start");
extern const uint8_t pb_voice_announce_end[] asm("_binary_announce_wav_end");
extern const uint8_t pb_voice_captured_start[] asm("_binary_captured_wav_start");
extern const uint8_t pb_voice_captured_end[] asm("_binary_captured_wav_end");
extern const uint8_t pb_voice_closer_start[] asm("_binary_closer_wav_start");
extern const uint8_t pb_voice_closer_end[] asm("_binary_closer_wav_end");
extern const uint8_t pb_voice_shutter_start[] asm("_binary_shutter_wav_start");  // tools/make_shutter.py で生成
extern const uint8_t pb_voice_shutter_end[] asm("_binary_shutter_wav_end");
