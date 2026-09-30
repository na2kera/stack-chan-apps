// StackChan K151 撮影ファーム。
// 各モジュールを組み立てて App に渡すだけ。初期化順は docs/design/step1-device.md §6 と
// step2b-device-edge.md §4.3 (Wi-Fi を最大 WIFI_BOOT_WAIT_MS 待ってから IDLE)。
// -DPHOTOBOOTH_NO_EDGE なら Wi-Fi に繋がず NullEdge (ステップ1の固定 QR) で動く。
#include <Arduino.h>
#include <M5StackChan.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "app/app.h"
#include "config.h"
#include "hal/audio.h"
#include "hal/camera.h"
#include "hal/head.h"
#include "hal/input.h"
#include "ui/screens.h"
#ifdef PHOTOBOOTH_NO_EDGE
#include "edge/null_edge.h"
#else
#include "net/http_edge_client.h"
#include "net/wifi_link.h"
#endif

namespace {

constexpr const char* TAG = "main";

hal::Camera camera;
hal::Head head;
hal::Audio audio;
hal::Input input;
#ifdef PHOTOBOOTH_NO_EDGE
edge::NullEdge edge_client;
#else
net::WifiLink wifi;
net::HttpEdgeClient edge_client(wifi);
#endif
app::App photobooth(camera, head, audio, edge_client);

}  // namespace

void setup() {
  const uint32_t t0 = millis();

  // 1. M5.begin() + 頭部タッチ + IO エキスパンダ + サーボ
  M5StackChan.begin();
  ESP_LOGI(TAG, "M5StackChan.begin done (%lu ms)", static_cast<unsigned long>(millis() - t0));

#ifndef PHOTOBOOTH_NO_EDGE
  // 1b. Wi-Fi の接続を先に始めておく (待つのは 5 の前)。
  const uint32_t wifi_t0 = millis();
  wifi.begin();
#endif

  // 2. スピーカー (マイクは止める)
  audio.speakerOn();

  // 3. 首を正面へ。応答が無ければ head.update() が異常を検出し、固定カメラとして続行する。
  head.begin(millis());

  // 4. カメラ。失敗したら ERROR から始める。
  const bool camera_ok = camera.begin();

#ifndef PHOTOBOOTH_NO_EDGE
  // 4b. net タスクを起動し、Wi-Fi を最大 WIFI_BOOT_WAIT_MS 待つ。繋がらなくても IDLE に入る
  //     (IDLE で「PC未接続」、タッチで診断画面)。
  edge_client.begin();
  if (!wifi.connected()) {
    ui::drawBootMessage("Wi-Fi接続中");
    while (!wifi.connected() && millis() - wifi_t0 < config::WIFI_BOOT_WAIT_MS) {
      head.update(millis());
      delay(50);
    }
  }
  ESP_LOGI(TAG, "wifi %s after %lu ms", wifi.connected() ? "connected" : "not connected",
           static_cast<unsigned long>(millis() - wifi_t0));
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
