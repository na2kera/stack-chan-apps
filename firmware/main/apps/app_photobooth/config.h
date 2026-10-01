/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の設定 (device/include/config.example.h のうちステップ1で使う分)。
//
// 鍵・Wi-Fi などの秘密はまだ無いのでコミットしてよい。ステップ2 (edge 接続) で秘密を足すときは
// 別ファイルに分けて .gitignore する。値の意味と初期値の根拠は docs/spec.md §2, §6.3, §9。
#pragma once

#include <cstdint>

namespace photobooth::config {

// ---- 撮影フロー ----
constexpr uint32_t COUNTDOWN_SEC      = 10;    // CAPTURE の長さ (spec §2 カウント)
constexpr uint32_t COMPOSE_TIMEOUT_MS = 5000;  // COMPOSE の上限 (spec §4)

// ---- 首振り (単位: 純正 Motion と同じ 1/10 度。250 = 25°) ----
// X = yaw (左右)、Y = pitch (純正の home = 0 が下向き、値が大きいほど上向き)。
// 方向の符号・可動域は実機で校正して書き換える (spec §6.3)。
constexpr int HEAD_X_MIN     = -250;  // 中心から左右 ±25°
constexpr int HEAD_X_MAX     = 250;
constexpr int HEAD_Y_MIN     = 250;   // 25°〜65°。公式推奨 5〜85° を越えない
constexpr int HEAD_Y_MAX     = 650;
constexpr int HEAD_X_NEUTRAL = 0;     // 正面姿勢 (実機で調整)
constexpr int HEAD_Y_NEUTRAL = 450;
constexpr int HEAD_STEP_MAX  = 30;    // 1 回の指示で動く最大角 (3°)
constexpr uint32_t HEAD_STEP_INTERVAL_MS = 500;  // 指示の最小間隔
constexpr int HEAD_SPEED     = 200;   // Motion::moveWithSpeed の speed (0..1000)。低め
// 指示後この時間を過ぎても isMoving() が true のままならサーボ応答なしとみなし、
// 首を止めて固定カメラとして続行する (spec §9 サーボエラー)
constexpr uint32_t HEAD_MOVE_TIMEOUT_MS = 3000;
// 首が止まったと判定してから、この時間が過ぎて取ったフレームだけを候補にする (ブレ防止。spec §4)。
// isMoving() は 100 ms ごとにしか問い合わせないので、その分の余裕も含む。
constexpr uint32_t HEAD_SETTLE_MS = 150;

// ---- カメラ ----
// プレビューの向き。純正は起動時に SetHMirror(false) にしているので、既定は純正のまま。
// 実機で鏡像・上下逆なら true にする (アプリを閉じるときに false へ戻す)。
constexpr bool CAMERA_HMIRROR = false;
constexpr bool CAMERA_VFLIP   = false;

// ---- 配布 URL (ステップ1では固定 QR の内容。ステップ4で gallery の応答に置き換わる) ----
constexpr const char* FIXED_PHOTO_URL = "https://example.com/p/fixed-demo-token";
constexpr const char* FIXED_SHARE_URL = "https://example.com/share/x";

}  // namespace photobooth::config
