// 画面部品: タイトル帯・ボタン帯・QR・折り返し文字列など。M5.Display にだけ依存する。
#pragma once

#include <M5GFX.h>

#include <cstdint>

namespace ui {

constexpr int16_t kScreenW = 320;
constexpr int16_t kScreenH = 240;
constexpr int16_t kTitleH = 32;       // 上: タイトル帯
constexpr int16_t kButtonBarH = 48;   // 下: ボタン帯
constexpr int16_t kButtonGap = 8;     // ボタン間隔 (左右端の余白も同じ)
constexpr int16_t kButtonPadY = 5;    // ボタン帯の中でのボタン上下余白
constexpr int kMaxButtons = 2;

// QR: 左に 168x168、右に 144px 幅の説明欄 (design §5)。
constexpr int16_t kQrSize = 168;
constexpr int16_t kQrMinModulePx = 160;  // QR 本体 (モジュール部分) の最小サイズ
constexpr int16_t kQrX = 4;
// タイトル帯 (32) + QR (168) + ボタン帯 (48) = 248 > 240 なので、QR はタイトル帯に 8px
// 食い込ませて置き、タイトル文字は右欄に寄せる (design との差分。README/PR 参照)。
constexpr int16_t kQrY = kScreenH - kButtonBarH - kQrSize;  // 24
constexpr int16_t kQrTextX = kQrX + kQrSize + 8;            // 180
constexpr int16_t kQrTextW = kScreenW - kQrTextX - 4;       // 136

struct Rect {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

namespace color {
uint16_t bg();
uint16_t text();
uint16_t titleBg();
uint16_t buttonBg();
uint16_t buttonText();
uint16_t overlayBg();
uint16_t overlayText();
uint16_t warn();
uint16_t muted();
}  // namespace color

namespace font {
const lgfx::IFont* small();   // lgfxJapanGothic_16
const lgfx::IFont* body();    // lgfxJapanGothic_20
const lgfx::IFont* heading(); // lgfxJapanGothic_24
const lgfx::IFont* large();   // lgfxJapanGothic_40
}  // namespace font

enum class Align : uint8_t { Left, Center };

// タイトル帯を描く。text_x を指定すると左寄せでその位置から描く (QR 画面用)。
void drawTitleBar(const char* title, int16_t text_x = 8);

// ボタン帯。labels は count 個 (1..kMaxButtons)。
void drawButtons(const char* const* labels, int count);
Rect buttonRect(int index, int count);
// (x, y) がボタン帯の何番目のボタンか。外れなら -1。
int hitButton(int x, int y, int count);

// rect 内に UTF-8 文字列を折り返して描く ('\n' と、行が溢れたときの直前の空白で改行)。
// 縦方向は rect の中央に揃える。
void drawTextBox(const char* text, const Rect& rect, const lgfx::IFont* f, uint16_t fg, uint16_t bg,
                 Align align);

// (x, y) に size x size の白地 QR を描く。モジュール部分が kQrMinModulePx 以上になる
// 最小の version を選ぶ。戻り値は選んだ version (描けなければ 0)。
int drawQr(const char* text, int16_t x, int16_t y, int16_t size);

// RGB565 (esp_camera のバイト順) の画像を (0, 0) に描く。exclude の矩形には描かない
// (その上に重ねる文字を毎フレーム描き直さずに済ませ、ちらつきを防ぐ)。
void drawFrameExcept(const uint16_t* pixels, int16_t w, int16_t h, const Rect* exclude);

}  // namespace ui
