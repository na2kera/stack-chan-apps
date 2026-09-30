#include "hal/audio.h"

#include <M5Unified.h>
#include <esp_log.h>

#include "config.h"

// board_build.embed_files で埋め込んだ WAV (platformio.ini)。
extern const uint8_t announce_wav_start[] asm("_binary_data_announce_wav_start");
extern const uint8_t announce_wav_end[] asm("_binary_data_announce_wav_end");
extern const uint8_t captured_wav_start[] asm("_binary_data_captured_wav_start");
extern const uint8_t captured_wav_end[] asm("_binary_data_captured_wav_end");
extern const uint8_t closer_wav_start[] asm("_binary_data_closer_wav_start");
extern const uint8_t closer_wav_end[] asm("_binary_data_closer_wav_end");

namespace hal {

namespace {
constexpr const char* TAG = "audio";
}

void Audio::speakerOn() {
  if (mode_ == Mode::Speaker) {
    return;
  }
  M5.Mic.end();
  M5.Speaker.begin();
  M5.Speaker.setVolume(config::SPEAKER_VOLUME);
  mode_ = Mode::Speaker;
  ESP_LOGI(TAG, "speaker on (volume %u)", static_cast<unsigned>(config::SPEAKER_VOLUME));
}

void Audio::micOn() {
  if (mode_ == Mode::Mic) {
    return;
  }
  M5.Speaker.end();
  M5.Mic.begin();
  mode_ = Mode::Mic;
  ESP_LOGI(TAG, "mic on");
}

bool Audio::play(const uint8_t* start, const uint8_t* end, const char* name) {
  speakerOn();
  const size_t len = static_cast<size_t>(end - start);
  const bool ok = M5.Speaker.playWav(start, len);
  if (ok) {
    ESP_LOGI(TAG, "play %s (%u bytes)", name, static_cast<unsigned>(len));
  } else {
    ESP_LOGE(TAG, "playWav %s failed (%u bytes)", name, static_cast<unsigned>(len));
  }
  return ok;
}

bool Audio::playAnnounce() { return play(announce_wav_start, announce_wav_end, "announce.wav"); }

bool Audio::playCaptured() { return play(captured_wav_start, captured_wav_end, "captured.wav"); }

bool Audio::playCloser() { return play(closer_wav_start, closer_wav_end, "closer.wav"); }

bool Audio::isPlaying() const { return mode_ == Mode::Speaker && M5.Speaker.isPlaying(); }

void Audio::stop() {
  if (mode_ == Mode::Speaker) {
    M5.Speaker.stop();
  }
}

}  // namespace hal
