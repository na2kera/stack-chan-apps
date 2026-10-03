/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の設定 (device/include/config.example.h の移植)。
//
// このファイルに秘密は無いのでコミットしてよい。edge の接続先と共有鍵は config_local.h
// (.gitignore 済み。見本は config_local.example.h) に分けてある。
// 値の意味と初期値の根拠は docs/spec.md §2, §6.3, §9。
#pragma once

#include <cstdint>

// edge (PC) と通信するビルドか (docs/design/fw-app-step2.md §2)。
//   - config_local.h があれば有効。無ければ無効 (ステップ1 と同じ単体動作・固定 URL)。
//   - `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` なら config_local.h があっても無効
//     (戻すときは `idf.py -DPHOTOBOOTH_NO_EDGE=0 build`。CMake のキャッシュに残るため)。
// config_local.h 自体は net/edge_config.h だけが include する (鍵を通信層の外に見せない)。
#if !defined(PHOTOBOOTH_NO_EDGE) && __has_include("config_local.h")
#define PHOTOBOOTH_EDGE_ENABLED 1
#else
#define PHOTOBOOTH_EDGE_ENABLED 0
#endif

namespace photobooth::config {

// ---- 撮影フロー ----
constexpr uint32_t COUNTDOWN_SEC      = 10;    // CAPTURE の長さ (spec §2 カウント)
constexpr uint32_t COMPOSE_TIMEOUT_MS = 5000;  // COMPOSE の上限 (spec §4)
constexpr uint32_t COMPOSE_STABLE_MS  = 1000;  // 顔が枠内に連続して入っている必要時間 (edge の判定つきのとき)
constexpr uint8_t MAX_FACES           = 4;     // 「4人までだよ」の人数 (edge の max_faces と揃える)
// SHUTTER (自動採用の直後のシャッター演出。docs/design/fw-app-step2.md「シャッター演出」)
constexpr uint32_t SHUTTER_FLASH_MS = 150;   // 画面を白く光らせる時間
constexpr uint32_t CAPTURED_HOLD_MS = 1000;  // 撮れた写真を止めて見せる最短の時間 (captured.wav が終わるまでは延びる)

// ---- edge (PC) との通信 (docs/protocol.md「タイムアウトと再試行」) ----
constexpr bool EDGE_ENABLED          = PHOTOBOOTH_EDGE_ENABLED != 0;
constexpr uint32_t EDGE_TIMEOUT_MS   = 3000;   // 1 リクエストのタイムアウト
constexpr uint32_t HELLO_INTERVAL_MS = 5000;   // 通信が無いときの hello の間隔 (接続確認)
constexpr uint32_t UPLOAD_WAIT_MS    = 15000;  // UPLOADING で写真の準備を待つ上限
constexpr uint8_t UPLOAD_RETRY       = 3;      // 保存 (review save) の送信回数の上限 (session ごと)

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
// これより小さい首の指示は出さない (1°)。サーボの不感帯より小さい移動は「止まった」と報告されず、
// 応答なし (HEAD_MOVE_TIMEOUT_MS) と誤判定されるため。
constexpr int HEAD_MIN_STEP = 10;
// HEAD_MOVE_TIMEOUT_MS を過ぎても動作中のとき、実際の角度が目標からこの範囲 (2°) なら止まったとみなす。
// 範囲外なら本当に応答なしとして首を止める。
constexpr int HEAD_SETTLE_TOLERANCE = 20;

// ---- カメラ ----
// プレビューの向き。純正は起動時に SetHMirror(false) にしているので、既定は純正のまま。
// 実機で鏡像・上下逆なら true にする (アプリを閉じるときに false へ戻す)。
constexpr bool CAMERA_HMIRROR = false;
constexpr bool CAMERA_VFLIP   = false;

// ---- 配布 URL (edge 無効ビルドのときだけ使う固定 QR の内容。有効なら edge の photo_ready の URL) ----
constexpr const char* FIXED_PHOTO_URL = "https://example.com/p/fixed-demo-token";
constexpr const char* FIXED_SHARE_URL = "https://example.com/share/x";

}  // namespace photobooth::config
