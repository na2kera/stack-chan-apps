#include "ui/screens.h"

#include <M5Unified.h>
#include <esp_log.h>

#include <cstdio>

namespace ui {

namespace {

constexpr const char* TAG = "ui";

// ---- IDLE の顔 ----
constexpr int16_t kEyeLX = 104;
constexpr int16_t kEyeRX = 216;
constexpr int16_t kEyeY = 92;
constexpr int16_t kEyeR = 18;
constexpr int16_t kMouthX = 160;
constexpr int16_t kMouthY = 112;
constexpr int16_t kMouthR = 44;

// ---- プレビューに重ねる領域 ----
// COMPOSE: 下端の案内帯
constexpr Rect kComposeBand{0, kScreenH - 40, kScreenW, 40};
// CAPTURE: 右上の人数・残り秒数
constexpr Rect kCaptureBox{kScreenW - 124, 0, 124, 84};

const Rect kBody{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - kButtonBarH - 8};
const Rect kBodyNoButtons{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - 8};

void clearWithTitle(const char* title) {
  M5.Display.fillScreen(color::bg());
  drawTitleBar(title);
}

void drawEye(int16_t cx, bool open) {
  auto& d = M5.Display;
  d.fillRect(cx - kEyeR - 1, kEyeY - kEyeR - 1, kEyeR * 2 + 3, kEyeR * 2 + 3, TFT_BLACK);
  if (open) {
    d.fillCircle(cx, kEyeY, kEyeR, TFT_WHITE);
  } else {
    d.fillRect(cx - kEyeR, kEyeY - 2, kEyeR * 2 + 1, 5, TFT_WHITE);
  }
}

}  // namespace

void begin() {
  auto& d = M5.Display;
  d.fillScreen(TFT_BLACK);
  ESP_LOGI(TAG, "display %dx%d", d.width(), d.height());
}

void drawIdle(bool pc_online, const char* warning) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_BLACK);
  drawEye(kEyeLX, true);
  drawEye(kEyeRX, true);
  // 口: 下側の弧 (LGFX の角度は 3 時方向 0°、時計回り)
  d.fillArc(kMouthX, kMouthY, kMouthR, kMouthR - 4, 30, 150, TFT_WHITE);

  d.setFont(font::body());
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString("写真を撮りたい、と言ってね", kScreenW / 2, 196);

  d.setFont(font::small());
  if (!pc_online) {
    d.setTextColor(color::muted(), TFT_BLACK);
    d.setTextDatum(textdatum_t::bottom_right);
    d.drawString("PC未接続", kScreenW - 6, kScreenH - 4);
  }
  if (warning != nullptr) {
    d.setTextColor(color::warn(), TFT_BLACK);
    d.setTextDatum(textdatum_t::bottom_left);
    d.drawString(warning, 6, kScreenH - 4);
  }
  d.endWrite();
}

void updateIdleEyes(bool open) {
  M5.Display.startWrite();
  drawEye(kEyeLX, open);
  drawEye(kEyeRX, open);
  M5.Display.endWrite();
}

void drawAnnounce(const char* title) {
  M5.Display.startWrite();
  clearWithTitle(title);
  drawTextBox("写真を撮るよ！ いい顔をしてね", kBodyNoButtons, font::heading(), color::text(),
              color::bg(), Align::Center);
  M5.Display.endWrite();
}

void drawComposeOverlay() {
  const Rect& r = kComposeBand;
  M5.Display.startWrite();
  M5.Display.fillRect(r.x, r.y, r.w, r.h, color::overlayBg());
  drawTextBox("みんな画面に入ってね", r, font::body(), color::overlayText(), color::overlayBg(),
              Align::Center);
  M5.Display.endWrite();
}

void drawComposeFrame(const uint16_t* pixels, int16_t w, int16_t h) {
  drawFrameExcept(pixels, w, h, &kComposeBand);
}

void drawCaptureOverlay(int remaining_sec, int face_count) {
  auto& d = M5.Display;
  const Rect& r = kCaptureBox;
  d.startWrite();
  d.fillRect(r.x, r.y, r.w, r.h, color::overlayBg());
  char buf[24];
  if (face_count < 0) {
    snprintf(buf, sizeof(buf), "人数 --");
  } else {
    snprintf(buf, sizeof(buf), "人数 %d", face_count);
  }
  d.setFont(font::body());
  d.setTextColor(color::overlayText(), color::overlayBg());
  d.setTextDatum(textdatum_t::top_center);
  d.drawString(buf, r.x + r.w / 2, r.y + 4);
  snprintf(buf, sizeof(buf), "%d", remaining_sec);
  d.setFont(font::large());
  d.drawString(buf, r.x + r.w / 2, r.y + 32);
  d.endWrite();
}

void drawCaptureFrame(const uint16_t* pixels, int16_t w, int16_t h) {
  drawFrameExcept(pixels, w, h, &kCaptureBox);
}

void drawPreviewPlaceholder() {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(TFT_DARKGREY);
  d.drawRect(4, 4, kScreenW - 8, kScreenH - 8, TFT_WHITE);
  d.setFont(font::body());
  d.setTextColor(TFT_WHITE, TFT_DARKGREY);
  d.setTextDatum(textdatum_t::middle_center);
  d.drawString("カメラ無効", kScreenW / 2, kScreenH / 2);
  d.endWrite();
}

void drawReview(const char* title, const uint16_t* pixels, int16_t w, int16_t h) {
  auto& d = M5.Display;
  d.startWrite();
  if (pixels != nullptr) {
    d.pushImage(0, 0, w, h, pixels);
    drawTitleBar(title);
    static const char* const kLabels[kReviewButtons] = {"保存する", "撮り直す"};
    drawButtons(kLabels, kReviewButtons);
  } else {
    clearWithTitle(title);
    drawTextBox("候補の写真がありません", kBody, font::body(), color::text(), color::bg(),
                Align::Center);
    static const char* const kLabels[kReviewButtonsNoCandidate] = {"撮り直す"};
    drawButtons(kLabels, kReviewButtonsNoCandidate);
  }
  d.endWrite();
}

void drawUploading(const char* title) {
  M5.Display.startWrite();
  clearWithTitle(title);
  drawTextBox("写真を準備中", kBodyNoButtons, font::heading(), color::text(), color::bg(),
              Align::Center);
  M5.Display.endWrite();
}

void drawPhotoQr(const char* title, const char* photo_url, const char* expires_at) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(color::bg());
  drawTitleBar(title, kQrTextX);
  const int version = drawQr(photo_url, kQrX, kQrY, kQrSize);

  d.setFont(font::small());
  d.setTextColor(color::text(), color::bg());
  d.setTextDatum(textdatum_t::top_left);
  d.drawString("削除予定", kQrTextX, kTitleH + 44);
  d.setFont(font::heading());
  d.drawString(expires_at, kQrTextX, kTitleH + 66);

  static const char* const kLabels[kPhotoQrButtons] = {"次へ", "撮り直す"};
  drawButtons(kLabels, kPhotoQrButtons);
  d.endWrite();
  ESP_LOGI(TAG, "photo QR drawn (version %d)", version);
}

void drawXQr(const char* title, const char* share_url) {
  auto& d = M5.Display;
  d.startWrite();
  d.fillScreen(color::bg());
  drawTitleBar(title, kQrTextX);
  const int version = drawQr(share_url, kQrX, kQrY, kQrSize);

  const Rect text{kQrTextX, kTitleH + 4, kQrTextW, kScreenH - kTitleH - kButtonBarH - 8};
  drawTextBox("保存した写真をXに添付してね", text, font::body(), color::text(), color::bg(),
              Align::Left);

  static const char* const kLabels[kXQrButtons] = {"戻る", "終了"};
  drawButtons(kLabels, kXQrButtons);
  d.endWrite();
  ESP_LOGI(TAG, "share QR drawn (version %d)", version);
}

void drawError(const char* title, const char* reason) {
  M5.Display.startWrite();
  clearWithTitle(title);
  drawTextBox(reason, kBody, font::heading(), color::text(), color::bg(), Align::Center);
  static const char* const kLabels[kErrorButtons] = {"再試行", "終了"};
  drawButtons(kLabels, kErrorButtons);
  M5.Display.endWrite();
}

}  // namespace ui
