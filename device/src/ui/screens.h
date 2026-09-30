// 状態ごとの描画 (docs/design/step1-device.md §5 ui)。M5.Display にだけ依存する。
//
// 静的な画面は状態に入ったときに draw*() を 1 回だけ呼び、変化する部分
// (残り秒数、目の開閉) だけを update*() で描き直す。
#pragma once

#include <cstdint>

#include "ui/widgets.h"

namespace ui {

// 各画面のボタン数 (App が hitButton() に渡す)。
constexpr int kReviewButtons = 2;          // 保存する / 撮り直す
constexpr int kReviewButtonsNoCandidate = 1;  // 撮り直す
constexpr int kPhotoQrButtons = 2;         // 次へ / 撮り直す
constexpr int kXQrButtons = 2;             // 戻る / 終了
constexpr int kErrorButtons = 2;           // 再試行 / 終了

void begin();

// IDLE: 黒地に顔と案内文。pc_online=false なら画面下に小さく「PC未接続」。
// warning は画面下左に出す警告 (nullptr なら無し)。
void drawIdle(bool pc_online, const char* warning);
void updateIdleEyes(bool open);

// ANNOUNCE
void drawAnnounce(const char* title);

// COMPOSE: プレビュー + 「みんな画面に入ってね」
void drawComposeOverlay();
void drawComposeFrame(const uint16_t* pixels, int16_t w, int16_t h);

// CAPTURE: プレビュー + 右上に人数と残り秒数。face_count < 0 は「--」。
void drawCaptureOverlay(int remaining_sec, int face_count);
void drawCaptureFrame(const uint16_t* pixels, int16_t w, int16_t h);

// カメラが無い (PHOTOBOOTH_NO_CAMERA / 取得失敗) ときのプレビュー枠。
void drawPreviewPlaceholder();

// REVIEW: 候補フレーム + 「保存する」「撮り直す」。pixels が nullptr なら「撮り直す」だけ。
void drawReview(const char* title, const uint16_t* pixels, int16_t w, int16_t h);

// UPLOADING: 「写真を準備中」
void drawUploading(const char* title);

// PHOTO_QR: 写真 QR + 削除予定時刻 + 「次へ」「撮り直す」
void drawPhotoQr(const char* title, const char* photo_url, const char* expires_at);

// X_QR: 投稿 QR + 「保存した写真をXに添付してね」 + 「戻る」「終了」
void drawXQr(const char* title, const char* share_url);

// ERROR: 理由 + 「再試行」「終了」
void drawError(const char* title, const char* reason);

}  // namespace ui
