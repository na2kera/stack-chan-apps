/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 画面部品: タイトル帯・ボタン帯・QR・折り返し文字列など (device/src/ui/widgets の LVGL 版)。
// レイアウトの数値は docs/design/step1-device.md §5 ui のまま。
// ここの関数は LVGL を直接触るので、呼び出し側 (view.cpp) が LvglLockGuard を取っていること。
#pragma once

#include <lvgl.h>
#include <smooth_lvgl.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace photobooth::view {

constexpr int32_t kScreenW    = 320;
constexpr int32_t kScreenH    = 240;
constexpr int32_t kTitleH     = 32;  // 上: タイトル帯
constexpr int32_t kButtonBarH = 48;  // 下: ボタン帯
constexpr int32_t kButtonGap  = 8;   // ボタン間隔 (左右端の余白も同じ)
constexpr int32_t kButtonPadY = 5;   // ボタン帯の中でのボタン上下余白
constexpr int kMaxButtons     = 2;

// QR 画面 (QR): タイトル帯の下に見出し 2 つ、その下に 132x132 の白地 QR を左右に並べる
// (docs/design/ui-review-two-qr.md §2)。左 = 写真ページ、右 = X の投稿画面。
constexpr int32_t kQrPairSize   = 132;
constexpr int32_t kQrLeftX      = 20;
constexpr int32_t kQrRightX     = 168;                         // 左 QR との間は 16
constexpr int32_t kQrHeadingH   = 24;                          // 各 QR の上の見出し (幅は QR と同じ)
constexpr int32_t kQrHeadingY   = kTitleH;                     // y=32..56
constexpr int32_t kQrPairY      = kQrHeadingY + kQrHeadingH;   // y=56..188。ボタン帯 (y=192〜) と重ならない
static_assert(kQrPairY + kQrPairSize <= kScreenH - kButtonBarH, "QR overlaps the button bar");
static_assert(kQrLeftX + kQrPairSize < kQrRightX, "QRs overlap each other");
static_assert(kQrRightX + kQrPairSize <= kScreenW, "right QR is off screen");

struct Rect {
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};

namespace color {
lv_color_t bg();
lv_color_t text();
lv_color_t titleBg();
lv_color_t buttonBg();
lv_color_t buttonText();
lv_color_t overlayBg();
lv_color_t overlayText();
lv_color_t warn();
lv_color_t muted();
}  // namespace color

namespace font {
const lv_font_t* body();   // 20px 日本語 (タイトル・本文・ボタン)
const lv_font_t* large();  // 28px 数字 (残り秒数)
}  // namespace font

enum class Align : uint8_t { Left, Center };

// 1 画面分の LVGL オブジェクトをまとめて持つ。破棄すると子から順に消える。
class Page {
public:
    explicit Page(lv_obj_t* parent);
    ~Page();
    Page(const Page&)            = delete;
    Page& operator=(const Page&) = delete;

    lv_obj_t* root()
    {
        return root_->get();
    }
    smooth_ui_toolkit::lvgl_cpp::Container& rootObj()
    {
        return *root_;
    }

    template <class T, class... Args>
    T& add(Args&&... args)
    {
        // 実際の型 T のまま破棄する (make_shared が T のデストラクタを直接呼ぶ)。
        // lvgl_cpp::Object は仮想デストラクタを持たず、Widget 側で vptr が足されるため、
        // Object* 経由で delete すると先頭アドレスがずれてヒープを壊す。
        auto obj = std::make_shared<T>(std::forward<Args>(args)...);
        T& ref   = *obj;
        objs_.push_back(std::move(obj));
        return ref;
    }

private:
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> root_;
    std::vector<std::shared_ptr<void>> objs_;
};

// 枠線・余白・スクロール・クリックを持たない矩形。
smooth_ui_toolkit::lvgl_cpp::Container& panel(Page& page, lv_obj_t* parent, const Rect& r, lv_color_t bg,
                                              lv_opa_t opa = LV_OPA_COVER);

// 上端のタイトル帯。
void titleBar(Page& page, const char* title);

// 下端のボタン帯。labels は count 個 (1..kMaxButtons)。押されると onClick(index)。
void buttonBar(Page& page, const char* const* labels, int count, const std::function<void(int)>& onClick);

// rect 内に文字列を折り返して置く (縦は rect の中央)。
smooth_ui_toolkit::lvgl_cpp::Label& textBox(Page& page, lv_obj_t* parent, const Rect& rect, const char* text,
                                            const lv_font_t* f, lv_color_t fg, Align align);

// (x, y) に size x size の白地 QR を置く。生成できなければ false (ログを出す)。
bool qr(Page& page, int32_t x, int32_t y, int32_t size, const char* text);

}  // namespace photobooth::view
