#!/usr/bin/env bash
# app_roulette の発話 (移植元はホストの TTS) を作り直す (本体は main/apps/shared/tools/make_voice.sh)。
# macOS の TTS (Kyoko) の仮音声を 24 kHz / mono / 16-bit の WAV にして assets/voice/ に置く。
# 埋め込みシンボルはファイル名から決まるので、photobooth と重ならないよう rl_ を付ける。
#
#   ./tools/make_voice.sh
#
# 依存: say (macOS 標準), ffmpeg
set -euo pipefail

cd "$(dirname "$0")/.."

../shared/tools/make_voice.sh assets/voice \
  rl_start "スタート" \
  rl_reach "リーチ"
