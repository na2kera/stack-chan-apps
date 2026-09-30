// 状態ごとの描画 (docs/design/step1-device.md §5 ui)。M5.Display にだけ依存する。
//
// 静的な画面は状態に入ったときに draw*() を 1 回だけ呼び、変化する部分
// (残り秒数、目の開閉) だけを update*() で描き直す。
#pragma once

#include <cstddef>
#include <cstdint>

#include "ui/widgets.h"

namespace ui {

// 各画面のボタン数 (App が hitButton() に渡す)。
constexpr int kReviewButtons = 2;          // 保存する / 撮り直す
constexpr int kReviewButtonsNoCandidate = 1;  // 撮り直す
constexpr int kPhotoQrButtons = 2;         // 次へ / 撮り直す
constexpr int kXQrButtons = 2;             // 戻る / 終了
constexpr int kErrorButtons = 2;           // 再試行 / 終了
constexpr int kDiagButtons = 2;            // 再接続 / 判定なしで撮影 (戻るは頭部タッチ)

void begin();

// 起動中の案内 (「Wi-Fi接続中」など)。黒地に 1 行。
void drawBootMessage(const char* text);

// IDLE: 黒地に顔と案内文。画面右下に小さく「PC接続中」/「PC未接続」。
// warning は画面下左に出す警告 (nullptr なら無し)。
void drawIdle(bool pc_online, const char* warning);
void updateIdleEyes(bool open);
// 右下の接続表示だけを描き直す。
void updateIdleStatus(bool pc_online);

// ANNOUNCE
void drawAnnounce(const char* title);

// COMPOSE / CAPTURE の下端の案内帯の既定の文言。hint に応じた文言は App が選ぶ。
constexpr const char* kBandDefault = "みんな画面に入ってね";

// COMPOSE: プレビュー + 下端の案内帯 (既定は「みんな画面に入ってね」)
void drawComposeOverlay(const char* band_text = kBandDefault);
void drawComposeFrame(const uint16_t* pixels, int16_t w, int16_t h);

// CAPTURE: プレビュー + 右上に「人数 face/target」と残り秒数。
// face_count < 0 は「人数 --」、target <= 0 は「人数 face/--」。
void drawCaptureOverlay(int remaining_sec, int face_count, int target = 0);
// CAPTURE の下端の案内帯 (hint があるときだけ出す)。
void drawCaptureBand(const char* band_text);
// with_band=true なら案内帯の領域にはフレームを描かない。
void drawCaptureFrame(const uint16_t* pixels, int16_t w, int16_t h, bool with_band = false);

// カメラが無い (PHOTOBOOTH_NO_CAMERA / 取得失敗) ときのプレビュー枠。
void drawPreviewPlaceholder();

// REVIEW で候補が無いときの文言。
constexpr const char* kReviewNoFace = "顔が見つからなかったよ";      // spec §9: 顔なしで時間切れ
constexpr const char* kReviewNoCandidate = "候補の写真がありません";  // 顔の有無が分からないとき

// REVIEW: 候補フレーム + 「保存する」「撮り直す」。pixels が nullptr なら empty_text と
// 「撮り直す」だけ。
void drawReview(const char* title, const uint16_t* pixels, int16_t w, int16_t h,
                const char* empty_text = kReviewNoCandidate);
// REVIEW: edge の候補 JPEG を全面に描き、タイトル帯と「保存する」「撮り直す」を重ねる。
// JPEG を描けなければ何も描かずに false。
bool drawReviewJpeg(const char* title, const uint8_t* jpeg, size_t len);

// UPLOADING: 「写真を準備中」。captured=true なら上に「撮れたよ」を出す (自動採用のとき)。
void drawUploading(const char* title, bool captured = false);

// PHOTO_QR: 写真 QR + 削除予定時刻 + 「次へ」「撮り直す」
void drawPhotoQr(const char* title, const char* photo_url, const char* expires_at);

// X_QR: 投稿 QR + 「保存した写真をXに添付してね」 + 「戻る」「終了」
void drawXQr(const char* title, const char* share_url);

// ERROR: 理由 + 「再試行」「終了」
void drawError(const char* title, const char* reason);

// DIAG: 診断 (複数行、'\n' 区切り) + 「再接続」「判定なしで撮影」。戻るは頭部タッチ。
void drawDiag(const char* title, const char* body);
// 診断の本文だけを描き直す。
void updateDiagBody(const char* body);

}  // namespace ui
