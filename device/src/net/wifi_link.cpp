#include "net/wifi_link.h"

#include <WiFi.h>
#include <esp_log.h>

#include <atomic>
#include <cstdio>

#include "config.h"

namespace net {

namespace {

constexpr const char* TAG = "wifi";

// 切断理由 (wifi_err_reason_t)。イベントタスクが書き、net / app タスクが読む。
std::atomic<uint16_t> g_last_reason{0};

void onEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      ESP_LOGI(TAG, "connected: ip %s rssi %d dBm", WiFi.localIP().toString().c_str(),
               static_cast<int>(WiFi.RSSI()));
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      g_last_reason.store(info.wifi_sta_disconnected.reason);
      ESP_LOGW(TAG, "disconnected: reason %u (%s)",
               static_cast<unsigned>(info.wifi_sta_disconnected.reason),
               WiFi.disconnectReasonName(
                   static_cast<wifi_err_reason_t>(info.wifi_sta_disconnected.reason)));
      break;
    default:
      break;
  }
}

}  // namespace

void WifiLink::begin() {
  if (started_) {
    return;
  }
  started_ = true;
  WiFi.onEvent(onEvent);
  WiFi.mode(WIFI_STA);
  // モデムスリープは往復遅延を数十〜数百 ms 増やすので切る (spec §6.1 の 2 fps のため)。
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  // SSID はログに出してよい (鍵・パスワードは出さない)。
  ESP_LOGI(TAG, "connecting to SSID \"%s\"", config::WIFI_SSID);
  WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);
}

void WifiLink::reconnect() {
  if (!started_) {
    begin();
    return;
  }
  ESP_LOGI(TAG, "reconnect requested");
  WiFi.disconnect(false);
  WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);
}

WifiLink::Status WifiLink::status() const {
  if (!started_) {
    return Status::Disconnected;
  }
  switch (WiFi.status()) {
    case WL_CONNECTED:
      return Status::Connected;
    case WL_IDLE_STATUS:
    case WL_DISCONNECTED:
    case WL_CONNECTION_LOST:  // 自動再接続中
      return Status::Connecting;
    default:  // WL_NO_SSID_AVAIL / WL_CONNECT_FAILED / WL_STOPPED など
      return Status::Disconnected;
  }
}

void WifiLink::ip(char* out, size_t len) const {
  if (!connected()) {
    snprintf(out, len, "-");
    return;
  }
  snprintf(out, len, "%s", WiFi.localIP().toString().c_str());
}

int WifiLink::rssi() const { return connected() ? static_cast<int>(WiFi.RSSI()) : 0; }

const char* WifiLink::lastDisconnectReason() const {
  const uint16_t r = g_last_reason.load();
  if (r == 0) {
    return "";
  }
  return WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(r));
}

}  // namespace net
