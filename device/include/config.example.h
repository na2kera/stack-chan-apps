// device 設定のサンプル。
//
//   cp include/config.example.h include/config.h
//
// して include/config.h を編集する。config.h は .gitignore 済みで、鍵や Wi-Fi 情報はこちらにだけ書く。
// 値の意味と初期値の根拠は docs/spec.md §2, §6.3, §9 を参照。
#pragma once

#include <cstdint>

namespace config {

// ---- ネットワーク (ステップ3以降で使用。ステップ1では未使用) ----
constexpr const char* WIFI_SSID     = "your-ssid";
constexpr const char* WIFI_PASSWORD = "your-password";
constexpr const char* EDGE_HOST     = "192.168.1.10";   // edge (PC) の LAN アドレス
constexpr uint16_t    EDGE_PORT     = 8765;
constexpr const char* DEVICE_ID     = "stackchan-01";
constexpr const char* EDGE_SHARED_KEY = "change-me";     // device と edge の共有鍵。Git に入れない

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
// 画像上のずれ → サーボ角の係数。符号は実機で確認して直す。
constexpr float HEAD_GAIN_X   = -0.05f; // image_dx(px) * gain = servo_dx(1/10度)
constexpr float HEAD_GAIN_Y   =  0.05f;

// ---- 画面 ----
constexpr uint32_t IDLE_BLINK_INTERVAL_MS = 3000; // IDLE の顔が瞬きする間隔
constexpr uint32_t IDLE_BLINK_MS          = 150;  // 目を閉じている時間

// ---- 音声 ----
constexpr uint8_t SPEAKER_VOLUME = 160;  // 0..255

// ---- 配布 URL (ステップ1では固定 QR の内容として使う。ステップ4で gallery の応答に置き換わる) ----
constexpr const char* FIXED_PHOTO_URL = "https://example.com/p/fixed-demo-token";
constexpr const char* FIXED_SHARE_URL = "https://example.com/share/x";

}  // namespace config
