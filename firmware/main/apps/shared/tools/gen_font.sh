#!/usr/bin/env bash
# 自作アプリ用の LVGL フォント (C 配列) を作る。app_photobooth / app_roulette の tools/gen_font.sh から呼ぶ。
#
# 純正同梱の font_puhui_basic_20_4 (ランチャー等が使う 20px) には、かな・漢字の一部
# (例: あ と ね 撮 写 …) が無いため、純正と同じ Alibaba PuHuiTi 系の
# managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf から、必要な文字だけを切り出す。
#
#   gen_font.sh <strings.h> <出力.c> <px> [--ascii]
#       strings.h の文字列リテラルに出てくる非 ASCII 文字を重複なしで拾う。
#       --ascii を付けると ASCII (0x20-0x7E) も入れる。
#   gen_font.sh --symbols <文字> <出力.c> <px> [--ascii]
#       strings.h の代わりに文字を直接指定する (数字だけのフォントなど)。
#
# 4bpp / 圧縮なし。firmware/ で idf.py build 済み (managed_components がある状態) で実行する。
# 依存: python3, npx (lv_font_conv 1.5.3 を取得する)
set -euo pipefail

usage() {
  echo "usage: $0 <strings.h | --symbols CHARS> <out.c> <px> [--ascii]" >&2
  exit 2
}

[[ $# -ge 3 ]] || usage
if [[ $1 == --symbols ]]; then
  [[ $# -ge 4 ]] || usage
  symbols=$2
  shift 2
else
  strings_h=$1
  shift
  if [[ ! -f "$strings_h" ]]; then
    echo "not found: $strings_h" >&2
    exit 1
  fi
  # strings.h の "..." の中身から非 ASCII 文字を重複なしで取り出す。
  symbols=$(python3 - "$strings_h" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8").read()
chars = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    chars.update(c for c in lit if ord(c) > 0x7E)
print("".join(sorted(chars)))
PY
)
  echo "non-ASCII symbols (${#symbols}): $symbols"
fi
out=$1
px=$2
ascii=0
if [[ ${3:-} == --ascii ]]; then
  ascii=1
elif [[ -n ${3:-} ]]; then
  usage
fi

fw_dir=$(cd "$(dirname "$0")/../../../.." && pwd)
ttf="$fw_dir/managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf"
if [[ ! -f "$ttf" ]]; then
  echo "not found: $ttf (run idf.py build once to fetch managed_components)" >&2
  exit 1
fi

# 生成ファイル先頭の Opts コメントにマシン固有の絶対パスが入らないよう、firmware/ からの相対パスで渡す。
out_dir=$(cd "$(dirname "$out")" && pwd)
out_abs="$out_dir/$(basename "$out")"
rel_out=${out_abs#"$fw_dir"/}
if [[ $rel_out == "$out_abs" ]]; then
  echo "output must be under $fw_dir: $out" >&2
  exit 1
fi
cd "$fw_dir"
rel_ttf=managed_components/78__xiaozhi-fonts/ttf/puhui-common.ttf
range=()
if [[ $ascii == 1 ]]; then
  range=(-r 0x20-0x7E)
fi
npx -y lv_font_conv@1.5.3 --no-compress --no-prefilter --force-fast-kern-format \
  --format lvgl --lv-include lvgl.h --font "$rel_ttf" \
  --size "$px" --bpp 4 ${range[@]+"${range[@]}"} --symbols "$symbols" -o "$rel_out"
echo "wrote $rel_out"
