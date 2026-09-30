#include "hal/audio.h"

#include <M5Unified.h>
#include <esp_log.h>

#ifdef PHOTOBOOTH_AUDIO_DIAGNOSTICS
#include <initializer_list>
#endif

#include "config.h"

// board_build.embed_files で埋め込んだ WAV (platformio.ini)。
extern const uint8_t announce_wav_start[] asm("_binary_data_announce_wav_start");
extern const uint8_t announce_wav_end[] asm("_binary_data_announce_wav_end");
extern const uint8_t captured_wav_start[] asm("_binary_data_captured_wav_start");
extern const uint8_t captured_wav_end[] asm("_binary_data_captured_wav_end");

namespace hal {

namespace {
constexpr const char* TAG = "audio";
}

void Audio::speakerOn() {
  if (mode_ == Mode::Speaker) {
    return;
  }
  M5.Mic.end();
  if (!M5.Speaker.begin()) {
    mode_ = Mode::Off;
    ESP_LOGE(TAG, "Speaker.begin failed");
    return;
  }
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
  if (mode_ != Mode::Speaker) {
    return false;
  }
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

bool Audio::isPlaying() const { return mode_ == Mode::Speaker && M5.Speaker.isPlaying(); }

void Audio::stop() {
  if (mode_ == Mode::Speaker) {
    M5.Speaker.stop();
  }
}

#ifdef PHOTOBOOTH_AUDIO_DIAGNOSTICS
void Audio::diagnose(const char* stage, bool restart_speaker) {
  if (restart_speaker) {
    M5.Speaker.end();
    mode_ = Mode::Off;
    speakerOn();
  }
  const auto spk = M5.Speaker.config();
  ESP_LOGI(TAG, "diag %s: board=%d speaker=%d I2S=%d BCK=%d WS=%d DATA=%d rate=%lu volume=%u",
           stage, M5.getBoard(), mode_ == Mode::Speaker, spk.i2s_port,
           spk.pin_bck, spk.pin_ws, spk.pin_data_out,
           static_cast<unsigned long>(spk.sample_rate), M5.Speaker.getVolume());

  // M5Unified 0.2.24 の CoreS3/StackChan コールバックと同じアドレス。
  // readRegister8() は失敗時も 0 を返すため、成否が分かる readRegister() を使う。
  for (uint8_t reg : {0x00, 0x01, 0x04, 0x05, 0x06, 0x0C}) {
    uint8_t data[2] = {};
    if (M5.In_I2C.readRegister(0x36, reg, data, sizeof(data), 400000)) {
      const unsigned value = (static_cast<unsigned>(data[0]) << 8) | data[1];
      ESP_LOGI(TAG, "diag %s: AW88298[0x%02x]=0x%04x", stage, reg, value);
    } else {
      ESP_LOGE(TAG, "diag %s: AW88298[0x%02x] read failed", stage, reg);
    }
  }
  uint8_t output = 0;
  if (M5.In_I2C.readRegister(0x58, 0x02, &output, 1, 400000)) {
    ESP_LOGI(TAG, "diag %s: AW9523[0x02]=0x%02x (speaker enable bit=%u)",
             stage, output, (output >> 2) & 1);
  } else {
    ESP_LOGE(TAG, "diag %s: AW9523[0x02] read failed", stage);
  }

  const uint32_t start = millis();
  const bool queued = mode_ == Mode::Speaker && M5.Speaker.tone(1000, 250);
  while (queued && M5.Speaker.isPlaying() && millis() - start < 1500) {
    delay(1);
  }
  const bool timed_out = queued && M5.Speaker.isPlaying();
  if (timed_out) {
    M5.Speaker.stop();
  }
  ESP_LOGI(TAG, "diag %s: tone queued=%d elapsed=%lu ms timeout=%d (audibility requires listening)",
           stage, queued, static_cast<unsigned long>(millis() - start), timed_out);
}
#endif

}  // namespace hal
