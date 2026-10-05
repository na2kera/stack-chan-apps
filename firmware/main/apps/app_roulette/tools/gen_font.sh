#!/usr/bin/env bash
# app_roulette の日本語フォントを作り直す (本体は main/apps/shared/tools/gen_font.sh)。
#
#   rl_font_jp_20.c : 20px / 4bpp。ASCII (0x20-0x7E) + view/strings.h の非 ASCII 文字
#
#   ./tools/gen_font.sh        (firmware/ で idf.py build 済み = managed_components がある状態で)
#
# 依存: python3, npx (lv_font_conv 1.5.3 を取得する)
set -euo pipefail

cd "$(dirname "$0")/.."

../shared/tools/gen_font.sh view/strings.h assets/rl_font_jp_20.c 20 --ascii
