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
  // 「もう少し寄ってね」(data/closer.wav)。edge の hint=closer のとき。
  bool playCloser();

  bool isPlaying() const;
  void stop();

  Mode mode() const { return mode_; }

 private:
  bool play(const uint8_t* start, const uint8_t* end, const char* name);

  Mode mode_ = Mode::Off;
};

}  // namespace hal
