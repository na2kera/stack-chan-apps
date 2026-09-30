// Wi-Fi (STA) の接続と再接続 (docs/design/step2b-device-edge.md §4)。
//
// 接続は Arduino core の自動再接続に任せ、ここでは状態の読み出しと手動の再接続だけを持つ。
// WiFi.h はこのファイルの .cpp と http_edge_client.cpp だけが include する (app は知らない)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace net {

class WifiLink {
 public:
  enum class Status : uint8_t { Disconnected, Connecting, Connected };

  // STA モードで config::WIFI_SSID に接続を始める (待たない)。
  void begin();
  // 切断してから接続し直す (待たない)。net タスクから呼ぶ。
  void reconnect();

  Status status() const;
  bool connected() const { return status() == Status::Connected; }

  // 診断用。接続していなければ ip は "-"、rssi は 0。
  void ip(char* out, size_t len) const;
  int rssi() const;
  // 最後に切断されたときの理由 (esp_wifi の reason 名)。無ければ ""。
  const char* lastDisconnectReason() const;

 private:
  bool started_ = false;
};

}  // namespace net
