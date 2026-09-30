#!/usr/bin/env bash
# セリフ音声の差し替え用スクリプト。
# macOS の TTS (Kyoko) で仮音声を作り、K151 で再生できる 16 kHz / mono / 16-bit WAV に変換する。
# 本番の録音音声に差し替えるときは、同じ形式の WAV を data/ に同名で置けばよい。
#
#   ./tools/make_voice.sh
#
# 依存: say (macOS 標準), ffmpeg
set -euo pipefail

cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

gen() {
  local name=$1 text=$2
  say -v Kyoko -o "$tmp/$name.aiff" "$text"
  ffmpeg -y -loglevel error -i "$tmp/$name.aiff" -ac 1 -ar 16000 -sample_fmt s16 "data/$name.wav"
  echo "wrote data/$name.wav"
}

gen announce "写真を撮るよ！ いい顔をしてね"
gen captured "撮れたよ"
