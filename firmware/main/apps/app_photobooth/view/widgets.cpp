/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "widgets.h"

#include <mooncake_log.h>

#include <algorithm>
#include <cstring>

#include "../assets/pb_assets.h"

using namespace smooth_ui_toolkit::lvgl_cpp;

namespace photobooth::view {

namespace {
constexpr const char* kTag = "PB-View";
}

// 色は独立ファーム版 (device/src/ui/widgets.cpp) と同じ。
namespace color {
lv_color_t bg()
{
    return lv_color_hex(0xFFFFFF);
}
lv_color_t text()
{
    return lv_color_hex(0x000000);
}
lv_color_t titleBg()
{
    return lv_color_make(255, 214, 153);
}
lv_color_t buttonBg()
{
    return lv_color_make(33, 110, 200);
}
lv_color_t buttonText()
{
    return lv_color_hex(0xFFFFFF);
}
lv_color_t overlayBg()
{
    return lv_color_hex(0x000000);
}
lv_color_t overlayText()
{
    return lv_color_hex(0xFFFFFF);
}
lv_color_t warn()
{
    // 独立ファーム版は黒地に (255,190,0)。こちらは白地なので読める濃さに落とした同系色。
    return lv_color_make(196, 120, 0);
}
lv_color_t muted()
{
    return lv_color_make(150, 150, 150);
}
}  // namespace color

namespace font {
const lv_font_t* body()
{
    return &pb_font_jp_20;
}
const lv_font_t* large()
{
    return &pb_font_num_28;
}
}  // namespace font

namespace {

void makeStatic(Object& o)
{
    o.removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    o.removeFlag(LV_OBJ_FLAG_CLICKABLE);
}

}  // namespace

Page::Page(lv_obj_t* parent)
{
    root_ = std::make_unique<Container>(parent);
    root_->setPos(0, 0);
    root_->setSize(kScreenW, kScreenH);
    root_->setRadius(0);
    root_->setBorderWidth(0);
    root_->setPaddingAll(0);
    root_->setBgColor(color::bg());
    root_->setBgOpa(LV_OPA_COVER);
    root_->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    root_->setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
}

Page::~Page()
{
    // 子を後ろから先に消してから root を消す (lvgl_cpp の各ラッパが自分のオブジェクトを消す)。
    while (!objs_.empty()) {
        objs_.pop_back();
    }
    root_.reset();
}

Container& panel(Page& page, lv_obj_t* parent, const Rect& r, lv_color_t bg, lv_opa_t opa)
{
    auto& c = page.add<Container>(parent);
    c.setPos(r.x, r.y);
    c.setSize(r.w, r.h);
    c.setRadius(0);
    c.setBorderWidth(0);
    c.setPaddingAll(0);
    c.setBgColor(bg);
    c.setBgOpa(opa);
    makeStatic(c);
    return c;
}

void titleBar(Page& page, const char* title)
{
    auto& bar = panel(page, page.root(), Rect{0, 0, kScreenW, kTitleH}, color::titleBg());
    auto& label = page.add<Label>(bar.get());
    label.setTextFont(font::body());
    label.setTextColor(color::text());
    label.setText(title);
    label.align(LV_ALIGN_LEFT_MID, 8, 0);
}

void buttonBar(Page& page, const char* const* labels, int count, const std::function<void(int)>& onClick)
{
    count = std::max(1, std::min(count, kMaxButtons));
    panel(page, page.root(), Rect{0, kScreenH - kButtonBarH, kScreenW, kButtonBarH}, color::bg());
    const int32_t w = (kScreenW - kButtonGap * (count + 1)) / count;
    const int32_t h = kButtonBarH - kButtonPadY * 2;
    const int32_t y = kScreenH - kButtonBarH + kButtonPadY;
    for (int i = 0; i < count; ++i) {
        auto& btn = page.add<Button>(page.root());
        btn.setPos(kButtonGap + i * (w + kButtonGap), y);
        btn.setSize(w, h);
        btn.setRadius(6);
        btn.setBgColor(color::buttonBg());
        btn.setBgOpa(LV_OPA_COVER);
        btn.setShadowWidth(0);
        btn.setBorderWidth(0);
        btn.label().setTextFont(font::body());
        btn.label().setTextColor(color::buttonText());
        btn.label().setText(labels[i]);
        btn.onClick().connect([onClick, i]() { onClick(i); });
    }
}

Label& textBox(Page& page, lv_obj_t* parent, const Rect& rect, const char* text, const lv_font_t* f, lv_color_t fg,
               Align align)
{
    auto& box = panel(page, parent, rect, color::bg(), LV_OPA_TRANSP);
    auto& label = page.add<Label>(box.get());
    label.setTextFont(f);
    label.setTextColor(fg);
    label.setWidth(rect.w);
    label.setLongMode(LV_LABEL_LONG_MODE_WRAP);
    label.setTextAlign(align == Align::Center ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT);
    label.setText(text);
    label.align(align == Align::Center ? LV_ALIGN_CENTER : LV_ALIGN_LEFT_MID, 0, 0);
    return label;
}

bool qr(Page& page, int32_t x, int32_t y, int32_t size, const char* text)
{
    // 白地の領域を先に敷き、その中央に QR を置く。lv_qrcode は ECC M で入る最小の version を選び、
    // 1 モジュールを size / モジュール数 (切り捨て) px に拡大して中央に描く。
    panel(page, page.root(), Rect{x, y, size, size}, lv_color_hex(0xFFFFFF));
    auto& code = page.add<Qrcode>(page.root());
    code.setSize(size);
    code.setDarkColor(lv_color_hex(0x000000));
    code.setLightColor(lv_color_hex(0xFFFFFF));
    code.setPos(x, y);
    makeStatic(code);
    const lv_result_t res = code.update(text);
    if (res != LV_RESULT_OK) {
        mclog::tagError(kTag, "QR generation failed (len {})", std::strlen(text));  // URL 自体はログに出さない
        return false;
    }
    mclog::tagInfo(kTag, "QR drawn ({}px, len {})", size, std::strlen(text));
    return true;
}

}  // namespace photobooth::view
