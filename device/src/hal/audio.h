// セリフ・効果音の再生と、マイク/スピーカーの排他切替 (docs/design/step1-device.md §5 audio)。
// K151 はマイクとスピーカーを同時に使えないので、必ずどちらか一方だけを有効にする。
#pragma once

#include <cstdint>

namespace hal {

class Audio {
 public:
  enum class Mode : uint8_t { Off, Speaker, Mic };

  // マイクを止めてスピーカーを有効にする。
  void speakerOn();
  // スピーカーを止めてマイクを有効にする (ステップ3の音声起動で使う)。
  void micOn();

  // 「写真を撮るよ！ いい顔をしてね」(data/announce.wav)。
  bool playAnnounce();
  // 「撮れたよ」(data/captured.wav)。シャッター音代わり。
  bool playCaptured();

  bool isPlaying() const;
  void stop();

#ifdef PHOTOBOOTH_AUDIO_DIAGNOSTICS
  // 起動時だけ使用。テスト音の終了を待つため、状態機械を開始する前に呼ぶ。
  void diagnose(const char* stage, bool restart_speaker = false);
#endif

  Mode mode() const { return mode_; }

 private:
  bool play(const uint8_t* start, const uint8_t* end, const char* name);

  Mode mode_ = Mode::Off;
};

}  // namespace hal
