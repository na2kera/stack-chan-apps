// device 設定のサンプル。
//
//   cp include/config.example.h include/config.h
//
// して include/config.h を編集する。config.h は .gitignore 済みで、鍵や Wi-Fi 情報はこちらにだけ書く。
// 値の意味と初期値の根拠は docs/spec.md §2, §6.3, §9 を参照。
#pragma once

#include <cstdint>

namespace config {

// ---- ネットワーク (ステップ2b から使用。-DPHOTOBOOTH_NO_EDGE のビルドでは未使用) ----
constexpr const char* WIFI_SSID     = "your-ssid";      // 2.4 GHz の SSID (ESP32-S3 は 5 GHz 非対応)
constexpr const char* WIFI_PASSWORD = "your-password";
constexpr const char* EDGE_HOST     = "192.168.1.10";   // edge (PC) の LAN アドレス。`ipconfig getifaddr en0`
constexpr uint16_t    EDGE_PORT     = 8765;
constexpr const char* DEVICE_ID     = "stackchan-01";   // edge の [auth] device_id と揃える
constexpr const char* EDGE_SHARED_KEY = "change-me";     // device と edge の共有鍵。Git に入れない
constexpr uint32_t EDGE_TIMEOUT_MS   = 3000;   // 1 リクエストのタイムアウト (protocol.md)
constexpr uint32_t HELLO_INTERVAL_MS = 5000;   // 通信が無いときの hello の間隔 (接続確認)
constexpr uint32_t WIFI_BOOT_WAIT_MS = 10000;  // 起動時に Wi-Fi 接続を待つ上限。繋がらなくても IDLE へ
constexpr uint32_t UPLOAD_WAIT_MS    = 15000;  // UPLOADING で写真の準備を待つ上限
constexpr uint8_t  UPLOAD_RETRY      = 3;      // 保存 (review save) の再試行の上限 (protocol.md)

// ---- 撮影フロー ----
constexpr uint32_t COUNTDOWN_SEC       = 10;   // CAPTURE の長さ (spec §2 カウント)
constexpr uint32_t COMPOSE_TIMEOUT_MS  = 5000; // COMPOSE の上限 (spec §4)
constexpr uint32_t COMPOSE_STABLE_MS   = 1000; // 顔が枠内に連続して入っている必要時間
constexpr uint8_t  MAX_FACES           = 4;    // 実装仮定C

// ---- 首振り (単位: BSP の 1/10 度。250 = 25°) ----
// 方向の符号・可動域は実機で校正して書き換える (spec §6.3)。
constexpr int HEAD_X_MIN      = -250;  // 中心から左右 ±25°
constexpr int HEAD_X_MAX      =  250;
constexpr int HEAD_Y_MIN      =  250;  // 25°〜65°。公式推奨 5〜85° を越えない
constexpr int HEAD_Y_MAX      =  650;
constexpr int HEAD_X_NEUTRAL  =    0;  // 正面姿勢 (実機で調整)
constexpr int HEAD_Y_NEUTRAL  =  450;
constexpr int HEAD_STEP_MAX   =   30;  // 1回の指示で動く最大角 (3°)
constexpr uint32_t HEAD_STEP_INTERVAL_MS = 500; // 指示の最小間隔
constexpr int HEAD_SPEED      =  200;  // BSP speed 0..1000、低め
// 指示後この時間を過ぎても isMoving() が true のままならサーボ応答なしとみなし、
// 首を止めて固定カメラとして続行する (spec §9 サーボエラー)
constexpr uint32_t HEAD_MOVE_TIMEOUT_MS = 3000;
// 画像上のずれ → サーボ角の係数は edge の config.toml [head] gain_x / gain_y にだけ置く。
// device は edge が返す servo_dx / servo_dy をそのまま nudge() に渡す (上の可動域・1 回の上限・
// 指示間隔でクランプする)。首が顔と逆に動くときは edge 側の符号を反転する。

// ---- 画面 ----
constexpr uint32_t IDLE_BLINK_INTERVAL_MS = 3000; // IDLE の顔が瞬きする間隔
constexpr uint32_t IDLE_BLINK_MS          = 150;  // 目を閉じている時間

// ---- 音声 ----
constexpr uint8_t SPEAKER_VOLUME = 160;  // 0..255

// ---- 配布 URL (ステップ1では固定 QR の内容として使う。ステップ4で gallery の応答に置き換わる) ----
constexpr const char* FIXED_PHOTO_URL = "https://example.com/p/fixed-demo-token";
constexpr const char* FIXED_SHARE_URL = "https://example.com/share/x";

}  // namespace config
