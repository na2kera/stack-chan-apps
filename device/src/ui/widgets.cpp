#include "ui/widgets.h"

#include <M5Unified.h>
#include <lgfx/utility/lgfx_qrcode.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace ui {

namespace color {
uint16_t bg() { return TFT_WHITE; }
uint16_t text() { return TFT_BLACK; }
uint16_t titleBg() { return M5.Display.color565(255, 214, 153); }
uint16_t buttonBg() { return M5.Display.color565(33, 110, 200); }
uint16_t buttonText() { return TFT_WHITE; }
uint16_t overlayBg() { return TFT_BLACK; }
uint16_t overlayText() { return TFT_WHITE; }
uint16_t warn() { return M5.Display.color565(255, 190, 0); }
uint16_t muted() { return M5.Display.color565(150, 150, 150); }
}  // namespace color

namespace font {
const lgfx::IFont* small() { return &fonts::lgfxJapanGothic_16; }
const lgfx::IFont* body() { return &fonts::lgfxJapanGothic_20; }
const lgfx::IFont* heading() { return &fonts::lgfxJapanGothic_24; }
const lgfx::IFont* large() { return &fonts::lgfxJapanGothic_40; }
}  // namespace font

namespace {

// 1 モジュールの最小ピクセル数。これ未満はスマホで読みにくいので選ばない。
constexpr int kQrMinThickness = 3;

// UTF-8 の 1 文字のバイト数。
size_t utf8Len(unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

std::vector<std::string> wrapLines(const char* text, int16_t max_w) {
  std::vector<std::string> lines;
  std::string line;
  size_t last_space = std::string::npos;  // line 内の最後の空白位置
  const char* p = text;
  while (*p != '\0') {
    if (*p == '\n') {
      lines.push_back(line);
      line.clear();
      last_space = std::string::npos;
      ++p;
      continue;
    }
    const size_t n = std::min(utf8Len(static_cast<unsigned char>(*p)), strlen(p));
    std::string candidate = line;
    candidate.append(p, n);
    if (!line.empty() && M5.Display.textWidth(candidate.c_str()) > max_w) {
      if (last_space != std::string::npos) {
        // 直前の空白で改行し、空白の後ろは次の行に回す。
        std::string rest = line.substr(last_space + 1);
        line.resize(last_space);
        lines.push_back(line);
        line = rest;
      } else {
        lines.push_back(line);
        line.clear();
      }
      last_space = std::string::npos;
      if (*p == ' ') {  // 行頭の空白は捨てる
        p += n;
        continue;
      }
    }
    if (*p == ' ') {
      last_space = line.size();
    }
    line.append(p, n);
    p += n;
  }
  if (!line.empty()) {
    lines.push_back(line);
  }
  return lines;
}

}  // namespace

void drawTitleBar(const char* title) {
  auto& d = M5.Display;
  d.fillRect(0, 0, kScreenW, kTitleH, color::titleBg());
  d.setFont(font::body());
  d.setTextColor(color::text(), color::titleBg());
  d.setTextDatum(textdatum_t::middle_left);
  d.drawString(title, 8, kTitleH / 2);
}

Rect buttonRect(int index, int count) {
  count = std::max(1, std::min(count, kMaxButtons));
  const int16_t w = (kScreenW - kButtonGap * (count + 1)) / count;
  const int16_t x = kButtonGap + index * (w + kButtonGap);
  const int16_t y = kScreenH - kButtonBarH + kButtonPadY;
  return Rect{x, y, w, static_cast<int16_t>(kButtonBarH - kButtonPadY * 2)};
}

void drawButtons(const char* const* labels, int count) {
  auto& d = M5.Display;
  d.fillRect(0, kScreenH - kButtonBarH, kScreenW, kButtonBarH, color::bg());
  d.setFont(font::body());
  d.setTextDatum(textdatum_t::middle_center);
  for (int i = 0; i < count && i < kMaxButtons; ++i) {
    const Rect r = buttonRect(i, count);
    d.fillRoundRect(r.x, r.y, r.w, r.h, 6, color::buttonBg());
    d.setTextColor(color::buttonText(), color::buttonBg());
    d.drawString(labels[i], r.x + r.w / 2, r.y + r.h / 2);
  }
}

int hitButton(int x, int y, int count) {
  if (y < kScreenH - kButtonBarH) {
    return -1;
  }
  for (int i = 0; i < count && i < kMaxButtons; ++i) {
    const Rect r = buttonRect(i, count);
    // ボタン帯の高さ全体と、間隔の半分までを当たりにする (指で押しやすく)。
    const Rect hit{static_cast<int16_t>(r.x - kButtonGap / 2),
                   static_cast<int16_t>(kScreenH - kButtonBarH),
                   static_cast<int16_t>(r.w + kButtonGap), kButtonBarH};
    if (hit.contains(x, y)) {
      return i;
    }
  }
  return -1;
}

void drawTextBox(const char* text, const Rect& rect, const lgfx::IFont* f, uint16_t fg, uint16_t bg,
                 Align align) {
  auto& d = M5.Display;
  d.setFont(f);
  d.setTextColor(fg, bg);
  const std::vector<std::string> lines = wrapLines(text, rect.w);
  const int16_t line_h = d.fontHeight() + 4;
  const int16_t total_h = static_cast<int16_t>(lines.size()) * line_h;
  int16_t y = rect.y + std::max<int16_t>(0, (rect.h - total_h) / 2);
  d.setTextDatum(align == Align::Center ? textdatum_t::top_center : textdatum_t::top_left);
  const int16_t x = align == Align::Center ? rect.x + rect.w / 2 : rect.x;
  for (const auto& line : lines) {
    d.drawString(line.c_str(), x, y);
    y += line_h;
  }
}

int drawQr(const char* text, int16_t x, int16_t y, int16_t size) {
  auto& d = M5.Display;
  d.fillRect(x, y, size, size, TFT_WHITE);
  for (uint8_t version = 1; version <= 40; ++version) {
    const int modules = 17 + 4 * version;
    const int thickness = size / modules;
    if (thickness < kQrMinThickness) {
      break;  // これ以上 version を上げるとモジュールが細かすぎて読めない
    }
    const int body_px = thickness * modules;
    if (body_px < kQrMinModulePx || body_px > size - kQrMinQuietPx * 2) {
      continue;  // 小さすぎる、または周りの白地が足りない
    }
    QRCode qr;
    std::vector<uint8_t> buf(lgfx_qrcode_getBufferSize(version));
    if (lgfx_qrcode_initText(&qr, buf.data(), version, ECC_LOW, text) != 0) {
      continue;  // この version には入らない
    }
    d.qrcode(text, x, y, size, version, false);
    return version;
  }
  // どの version でも条件を満たせない (URL が長すぎる)。読めるかは実機次第で最善を描く。
  d.qrcode(text, x, y, size, 1, false);
  return 0;
}

void drawFrameExcept(const uint16_t* pixels, int16_t w, int16_t h, const Rect* exclude) {
  auto& d = M5.Display;
  if (exclude == nullptr) {
    d.pushImage(0, 0, w, h, pixels);
    return;
  }
  const Rect& e = *exclude;
  // exclude の上・下・左・右の 4 領域だけにクリップして描く。
  const Rect parts[4] = {
      {0, 0, w, e.y},
      {0, static_cast<int16_t>(e.y + e.h), w, static_cast<int16_t>(h - (e.y + e.h))},
      {0, e.y, e.x, e.h},
      {static_cast<int16_t>(e.x + e.w), e.y, static_cast<int16_t>(w - (e.x + e.w)), e.h},
  };
  for (const Rect& r : parts) {
    if (r.w <= 0 || r.h <= 0) continue;
    d.setClipRect(r.x, r.y, r.w, r.h);
    d.pushImage(0, 0, w, h, pixels);
  }
  d.clearClipRect();
}

}  // namespace ui
