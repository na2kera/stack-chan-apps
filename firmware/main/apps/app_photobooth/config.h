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

// edge と通信するビルドか (docs/design/fw-app-step2.md §2)。
//   - config_local.h があれば有効。無ければ無効 (ステップ1 と同じ単体動作・固定 URL)。
//   - `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` なら config_local.h があっても無効
//     (戻すときは `idf.py -DPHOTOBOOTH_NO_EDGE=0 build`。CMake のキャッシュに残るため)。
// config_local.h 自体は net/edge_config.h だけが include する (鍵を通信層の外に見せない)。
#if !defined(PHOTOBOOTH_NO_EDGE) && __has_include("config_local.h")
#define PHOTOBOOTH_EDGE_ENABLED 1
#else
#define PHOTOBOOTH_EDGE_ENABLED 0
#endif

// frame を JPEG にして送るか (docs/design/step6-cloud-device.md §3.3)。既定は JPEG。
//   - `idf.py -DPHOTOBOOTH_FRAME_RGB565=1 build` で RGB565 のまま送る (6b までと同じ)。
//     戻すときは `idf.py -DPHOTOBOOTH_FRAME_RGB565=0 build` (CMake のキャッシュに残るため)。
//   - CMake 変数は firmware/main/CMakeLists.txt がコンパイル定義に渡す。
#if defined(PHOTOBOOTH_FRAME_RGB565) && PHOTOBOOTH_FRAME_RGB565
#define PHOTOBOOTH_FRAME_FORMAT_JPEG 0
#else
#define PHOTOBOOTH_FRAME_FORMAT_JPEG 1
#endif

// 試験用フック。`idf.py -DPHOTOBOOTH_TEST_HOOKS=1 ...` のときだけ有効 (製品ファームでは 0)。
#ifndef PHOTOBOOTH_TEST_HOOKS
#define PHOTOBOOTH_TEST_HOOKS 0
#endif

// 試験 22 (符号化失敗の注入) のデバッグ定義。`idf.py -DPHOTOBOOTH_TEST_HOOKS=1 -DPHOTOBOOTH_JPEG_FAIL_EVERY=N build`
// で、N 回に 1 回符号化を失敗扱いにする (エンコーダを呼ばずに、確保失敗と同じ経路で破棄する)。0 (既定) で無効。
// PHOTOBOOTH_TEST_HOOKS なしでは使えない (CMake と下の static_assert で止める)。
#ifndef PHOTOBOOTH_JPEG_FAIL_EVERY
#define PHOTOBOOTH_JPEG_FAIL_EVERY 0
#endif

namespace photobooth::config {

// ---- 撮影フロー ----
constexpr uint32_t COUNTDOWN_SEC      = 10;    // CAPTURE の長さ (spec §2 カウント)
constexpr uint32_t COMPOSE_TIMEOUT_MS = 5000;  // COMPOSE の上限 (spec §4)
constexpr uint32_t COMPOSE_STABLE_MS  = 1000;  // 顔が枠内に連続して入っている必要時間 (edge の判定つきのとき)
constexpr uint8_t MAX_FACES           = 4;     // 「4人までだよ」の人数 (edge の max_faces と揃える)
// SHUTTER (自動採用の直後のシャッター演出。docs/design/fw-app-step2.md「シャッター演出」)
constexpr uint32_t SHUTTER_FLASH_MS = 150;   // 画面を白く光らせる時間
// SHUTTER で撮れた写真 (候補 JPEG) を取りに行くときの期限。1 回だけ試し、送り直さない。
// この依頼の後ろに save が並ぶので、短くして save を待たせない (codex レビュー)。
// インターネット越し (クラウドの edge) の往復を見込んで 1.5 秒 (docs/design/step6-cloud-device.md §3.2)。
constexpr uint32_t SHUTTER_CANDIDATE_TIMEOUT_MS = 1500;
constexpr uint32_t CAPTURED_HOLD_MS = 1000;  // 撮れた写真を止めて見せる最短の時間 (captured.wav が終わるまでは延びる)

// ---- edge との通信 (docs/protocol.md「タイムアウトと再試行」、docs/design/step6-cloud-device.md §3.2) ----
constexpr bool EDGE_ENABLED          = PHOTOBOOTH_EDGE_ENABLED != 0;
constexpr uint32_t EDGE_TIMEOUT_MS   = 3000;   // 1 リクエストのタイムアウト (hello と SHUTTER の候補以外)
// hello の 1 試行の期限。TLS ハンドシェイク + Worker + cold start を見込む。通信失敗なら 1 回だけ送り直す
// (全体で約 16 秒)。接続後の応答待ちで切れたら「準備中」として扱い、次の hello で再試行する。
constexpr uint32_t HELLO_TIMEOUT_MS  = 8000;
constexpr uint32_t HELLO_INTERVAL_MS = 5000;   // 通信が無いときの hello の間隔 (接続確認)
// 「準備中」(Starting) が続いている間、待機画面のタッチを無視する長さ。これ以降のタッチは診断画面を開く
// (5xx が続く misconfigured / upstream_error でも再接続・判定なしの撮影に入れるように)。hello の操作全体の
// 上限 (約 16 秒) より少し長い。
constexpr uint32_t STARTING_TOUCH_IGNORE_MS = 20000;
constexpr uint32_t UPLOAD_WAIT_MS    = 15000;  // UPLOADING で写真の準備を待つ上限
constexpr uint8_t UPLOAD_RETRY       = 3;      // 保存 (review save) の送信回数の上限 (session ごと)

// ---- frame の形式 (docs/design/step6-cloud-device.md §3.3) ----
constexpr bool FRAME_FORMAT_JPEG       = PHOTOBOOTH_FRAME_FORMAT_JPEG != 0;  // false なら RGB565 LE のまま
constexpr uint8_t FRAME_JPEG_QUALITY   = 80;  // edge の候補 JPEG と同じ。サイズの上限は設けない
constexpr uint32_t JPEG_FAIL_EVERY     = PHOTOBOOTH_JPEG_FAIL_EVERY;  // 0 = 注入しない (デバッグ用)
constexpr bool TEST_HOOKS              = PHOTOBOOTH_TEST_HOOKS != 0;
static_assert(JPEG_FAIL_EVERY == 0 || TEST_HOOKS,
              "PHOTOBOOTH_JPEG_FAIL_EVERY は試験用。PHOTOBOOTH_TEST_HOOKS=1 のときだけ使える");

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
// HEAD_MOVE_TIMEOUT_MS を過ぎても動作中のとき、実際の角度が目標からこの範囲 (5°) なら止まったとみなす。
// 実機で、目標から約 4° ずれた位置で寄り切れずに「動作中」のままになることがあった (不感帯・機構の引っかかり)。
constexpr int HEAD_SETTLE_TOLERANCE = 50;
// 範囲外で止まらないときは、この時間は首の指示を出さずに待ち、その後また受け付ける (1 回で諦めない)。
// 待っている間に来た最新の指示は覚えておき、再開したときに送る。
constexpr uint32_t HEAD_FAULT_RETRY_MS = 2000;
// 応答なしがこの回数続いたら、そのセッションの間は首を止めて固定カメラにする (spec §9)。
constexpr int HEAD_FAULT_LIMIT = 3;

// ---- カメラ ----
// プレビューの向き。純正は起動時に SetHMirror(false) にしているので、既定は純正のまま。
// 実機で鏡像・上下逆なら true にする (アプリを閉じるときに false へ戻す)。
constexpr bool CAMERA_HMIRROR = false;
constexpr bool CAMERA_VFLIP   = false;

// ---- 配布 URL (edge 無効ビルドのときだけ使う固定 QR の内容。有効なら edge の photo_ready の URL) ----
constexpr const char* FIXED_PHOTO_URL = "https://example.com/p/fixed-demo-token";
constexpr const char* FIXED_SHARE_URL = "https://example.com/share/x";

}  // namespace photobooth::config
