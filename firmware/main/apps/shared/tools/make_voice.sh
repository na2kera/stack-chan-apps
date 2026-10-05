#!/usr/bin/env bash
# 自作アプリのセリフ音声を作る。app_photobooth / app_roulette の tools/make_voice.sh から呼ぶ。
# macOS の TTS (Kyoko) で仮音声を作り、コーデックの出力形式
# (24 kHz / mono / 16-bit PCM WAV。main/hal/board/config.h の AUDIO_OUTPUT_SAMPLE_RATE) に変換する。
# 本番の録音に差し替えるときは、同じ形式の WAV を同名で置けばよい。
# WAV はビルド時に main/CMakeLists.txt の EMBED_FILES でファームに埋め込まれる
# (シンボル名はファイル名から決まるので、アプリごとに重ならない名前にすること)。
#
#   make_voice.sh <出力ディレクトリ> <名前> <文言> [<名前> <文言>...]
#       <出力ディレクトリ>/<名前>.wav を作る。
#
# 依存: say (macOS 標準), ffmpeg
set -euo pipefail

if [[ $# -lt 3 || $((($# - 1) % 2)) -ne 0 ]]; then
  echo "usage: $0 <out_dir> <name> <text> [<name> <text>...]" >&2
  exit 2
fi
out_dir=$1
shift
mkdir -p "$out_dir"

rate=24000
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

while [[ $# -gt 0 ]]; do
  name=$1 text=$2
  shift 2
  say -v Kyoko -o "$tmp/$name.aiff" "$text"
  # -map_metadata -1 / -bitexact: LIST チャンクを付けず、再生成しても同じバイト列になるようにする
  ffmpeg -y -loglevel error -i "$tmp/$name.aiff" -map_metadata -1 -bitexact \
    -ac 1 -ar "$rate" -sample_fmt s16 "$out_dir/$name.wav"
  echo "wrote $out_dir/$name.wav"
done
