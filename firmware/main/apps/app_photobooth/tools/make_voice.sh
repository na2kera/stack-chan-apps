#!/usr/bin/env bash
# app_photobooth のセリフ音声を作り直す (device/tools/make_voice.sh の移植)。
# macOS の TTS (Kyoko) で仮音声を作り、コーデックの出力形式
# (24 kHz / mono / 16-bit PCM WAV。main/hal/board/config.h の AUDIO_OUTPUT_SAMPLE_RATE) に変換する。
# 本番の録音に差し替えるときは、同じ形式の WAV を assets/voice/ に同名で置けばよい。
# WAV はビルド時に main/CMakeLists.txt の EMBED_FILES でファームに埋め込まれる。
#
#   ./tools/make_voice.sh
#
# 依存: say (macOS 標準), ffmpeg
set -euo pipefail

cd "$(dirname "$0")/.."
rate=24000
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

gen() {
  local name=$1 text=$2
  say -v Kyoko -o "$tmp/$name.aiff" "$text"
  # -map_metadata -1 / -bitexact: LIST チャンクを付けず、再生成しても同じバイト列になるようにする
  ffmpeg -y -loglevel error -i "$tmp/$name.aiff" -map_metadata -1 -bitexact \
    -ac 1 -ar "$rate" -sample_fmt s16 "assets/voice/$name.wav"
  echo "wrote assets/voice/$name.wav"
}

gen announce "写真を撮るよ！ いい顔をしてね"
gen captured "撮れたよ"
gen closer "もう少し寄ってね"
