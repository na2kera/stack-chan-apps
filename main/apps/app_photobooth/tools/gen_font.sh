#!/usr/bin/env bash
# app_photobooth の日本語フォントを作り直す。
#
# 純正同梱の font_puhui_basic_20_4 (ランチャー等が使う 20px) には、かな・漢字の一部
# (例: あ と ね 撮 写 …) が無いため、純正と同じ Alibaba PuHuiTi 系の
# managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf から、
# view/strings.h の文字列リテラルに出てくる文字 + ASCII だけを切り出した LVGL フォントを作る。
#
#   pb_font_jp_20.c  : 20px / 4bpp。ASCII (0x20-0x7E) + strings.h の非 ASCII 文字
#   pb_font_num_48.c : 48px / 4bpp。CAPTURE の残り秒数用 (数字と '-' だけ)
#
#   ./tools/gen_font.sh        (firmware/ で idf.py build 済み = managed_components がある状態で)
#
# 依存: python3, npx (lv_font_conv 1.5.3 を取得する)
set -euo pipefail

cd "$(dirname "$0")/.."
app_dir=$(pwd)
fw_dir=$(cd ../../.. && pwd)
ttf="$fw_dir/managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf"
if [[ ! -f "$ttf" ]]; then
  echo "not found: $ttf (run idf.py build once to fetch managed_components)" >&2
  exit 1
fi

# strings.h の "..." の中身から非 ASCII 文字を重複なしで取り出す。
symbols=$(python3 - "$app_dir/view/strings.h" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
chars = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    chars.update(c for c in lit if ord(c) > 0x7E)
print("".join(sorted(chars)))
PY
)
echo "non-ASCII symbols (${#symbols}): $symbols"

# 生成ファイル先頭の Opts コメントにマシン固有の絶対パスが入らないよう、firmware/ からの相対パスで渡す。
cd "$fw_dir"
rel_ttf=managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf
out=main/apps/app_photobooth/assets
conv() {
  npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --force-fast-kern-format \
    --format lvgl --lv-include lvgl.h --font "$rel_ttf" "$@"
}

conv --size 20 --bpp 4 -r 0x20-0x7E --symbols "$symbols" -o "$out/pb_font_jp_20.c"
conv --size 48 --bpp 4 --symbols "0123456789-" -o "$out/pb_font_num_48.c"
echo "wrote assets/pb_font_jp_20.c assets/pb_font_num_48.c"
