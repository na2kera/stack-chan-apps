#!/usr/bin/env bash
# app_photobooth のセリフ音声を作り直す (本体は main/apps/shared/tools/make_voice.sh)。
# macOS の TTS (Kyoko) の仮音声を 24 kHz / mono / 16-bit の WAV にして assets/voice/ に置く。
# シャッター音 (shutter.wav) は tools/make_shutter.py で作る。
#
#   ./tools/make_voice.sh
#
# 依存: say (macOS 標準), ffmpeg
set -euo pipefail

cd "$(dirname "$0")/.."

../shared/tools/make_voice.sh assets/voice \
  announce "写真を撮るよ！ いい顔をしてね" \
  captured "撮れたよ" \
  closer "もう少し寄ってね"
