// StackChan K151 撮影ファーム (ステップ1)。
// 各モジュールを組み立てて App に渡すだけ。初期化順は docs/design/step1-device.md §6。
#include <Arduino.h>
#include <M5StackChan.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "app/app.h"
#include "edge/null_edge.h"
#include "hal/audio.h"
#include "hal/camera.h"
#include "hal/head.h"
#include "hal/input.h"

namespace {

constexpr const char* TAG = "main";

hal::Camera camera;
hal::Head head;
hal::Audio audio;
hal::Input input;
edge::NullEdge edge_client;
app::App photobooth(camera, head, audio, edge_client);

}  // namespace

void setup() {
  const uint32_t t0 = millis();

  // 1. M5.begin() + 頭部タッチ + IO エキスパンダ + サーボ
  M5StackChan.begin();
  ESP_LOGI(TAG, "M5StackChan.begin done (%lu ms)", static_cast<unsigned long>(millis() - t0));

  // 2. スピーカー (マイクは止める)
  audio.speakerOn();
#ifdef PHOTOBOOTH_AUDIO_DIAGNOSTICS
  audio.diagnose("before-camera");
#endif

  // 3. 首を正面へ。応答が無ければ head.update() が異常を検出し、固定カメラとして続行する。
  head.begin(millis());

  // 4. カメラ。失敗したら ERROR から始める。
  const bool camera_ok = camera.begin();
#ifdef PHOTOBOOTH_AUDIO_DIAGNOSTICS
  audio.diagnose("after-camera");
  audio.diagnose("after-speaker-restart", true);
#endif

  // 5. 状態機械
  photobooth.begin(millis(), camera_ok);

  ESP_LOGI(TAG, "boot done in %lu ms (camera %s%s, free heap %u, free PSRAM %u)",
           static_cast<unsigned long>(millis() - t0), camera_ok ? "ok" : "FAILED",
           hal::Camera::disabled() ? " [PHOTOBOOTH_NO_CAMERA]" : "",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void loop() {
  const hal::Event ev = input.poll();
  photobooth.update(ev, millis());
  delay(1);
}
