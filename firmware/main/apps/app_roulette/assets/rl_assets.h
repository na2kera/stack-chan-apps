/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_roulette の埋め込みリソース。
//   - フォント / アイコン / リール: 同じディレクトリの .c (tools/gen_font.sh, tools/make_icon.py,
//     tools/make_reel.py で生成)
//   - 発話 WAV: voice/*.wav を main/CMakeLists.txt の EMBED_FILES で埋め込む
//     (シンボル名は ESP-IDF の規則で _binary_<ファイル名>_start / _end)
#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>

LV_FONT_DECLARE(rl_font_jp_20);  // 状態の 1 行 (ASCII + view/strings.h の文字)
LV_IMAGE_DECLARE(icon_roulette);
LV_IMAGE_DECLARE(rl_reel);  // 72x288 RGB565。上から 技育展 / 技育祭 / 技育博 / 技育CAMP

extern const uint8_t rl_voice_start_start[] asm("_binary_rl_start_wav_start");  // 「スタート」
extern const uint8_t rl_voice_start_end[] asm("_binary_rl_start_wav_end");
extern const uint8_t rl_voice_reach_start[] asm("_binary_rl_reach_wav_start");  // 「リーチ」
extern const uint8_t rl_voice_reach_end[] asm("_binary_rl_reach_wav_end");
