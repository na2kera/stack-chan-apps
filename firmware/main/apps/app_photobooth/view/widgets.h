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

// QR 画面 (PHOTO_QR / X_QR) はタイトル帯を持たない。
// 左上に 184x184 の白地 QR 領域、右欄の 1 行目に状態名、その下に説明文を置く (design §5)。
constexpr int32_t kQrSize  = 184;
constexpr int32_t kQrX     = 8;
constexpr int32_t kQrY     = 4;                    // y=4..188。ボタン帯 (y=192〜) と重ならない
constexpr int32_t kQrTextX = kQrX + kQrSize + 8;   // 200
constexpr int32_t kQrTextW = kScreenW - kQrTextX - 4;  // 116
static_assert(kQrY + kQrSize <= kScreenH - kButtonBarH, "QR overlaps the button bar");

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
