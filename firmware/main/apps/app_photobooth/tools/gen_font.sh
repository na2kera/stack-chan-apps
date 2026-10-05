#!/usr/bin/env bash
# app_photobooth の日本語フォントを作り直す (本体は main/apps/shared/tools/gen_font.sh)。
#
#   pb_font_jp_20.c  : 20px / 4bpp。ASCII (0x20-0x7E) + view/strings.h の非 ASCII 文字
#   pb_font_num_28.c : 28px / 4bpp。CAPTURE の残り秒数用 (数字と '-' だけ)
#
#   ./tools/gen_font.sh        (firmware/ で idf.py build 済み = managed_components がある状態で)
#
# 依存: python3, npx (lv_font_conv 1.5.3 を取得する)
set -euo pipefail

cd "$(dirname "$0")/.."
gen=../shared/tools/gen_font.sh

"$gen" view/strings.h assets/pb_font_jp_20.c 20 --ascii
"$gen" --symbols "0123456789-" assets/pb_font_num_28.c 28
