/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 画面に出す文言 (docs/design/app-roulette.md §5)。
//
// 日本語フォント assets/rl_font_jp_20.c はこのファイルの文字列リテラルに含まれる文字 + ASCII だけから
// 作っている。文言を変えたら tools/gen_font.sh を実行してフォントを作り直すこと
// (作り直さないと、増えた文字が表示されない)。
#pragma once

#include "../game/slot.h"

namespace roulette::str {

// 状態の 1 行 (20px で 320px に収まる長さ)
inline constexpr const char* kReady    = "タップでスタート";
inline constexpr const char* kSpinning = "タップで止める";
inline constexpr const char* kWin      = "%s がそろった！";  // %s = ロゴ名 (「揃」はフォント元の puhui-common.ttf に無いのでかな)
inline constexpr const char* kLose     = "はずれ タップでもう一度";

// ロゴ名。並びはリールのシート (tools/make_reel.py の ORDER) と同じ。
inline constexpr const char* kSymbolNames[game::kSymbolCount] = {
    "技育展",
    "技育祭",
    "技育博",
    "技育CAMP",
};

}  // namespace roulette::str
