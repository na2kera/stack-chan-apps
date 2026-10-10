/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <hal/hal.h>
#include <mooncake_log.h>

#include <cmath>
#include <cstdio>

#include "../../shared/hw/input.h"
#include "../assets/rl_assets.h"
#include "strings.h"

using namespace smooth_ui_toolkit::lvgl_cpp;

namespace roulette::view {

namespace {

constexpr const char* kTag = "RL-View";

// 画面 (docs/design/app-roulette.md §5)
constexpr int32_t kScreenW      = 320;
constexpr int32_t kScreenH      = 240;
constexpr int32_t kSymbol       = static_cast<int32_t>(game::kSymbolSize);   // 72
constexpr int32_t kStrip        = static_cast<int32_t>(game::kStripHeight);  // 288
constexpr int32_t kWindowH      = kSymbol * 3;                               // 216 (見える 3 行)
constexpr int32_t kReelX[game::kReelCount] = {40, 124, 208};
constexpr int32_t kPaylineX     = 26;
constexpr int32_t kPaylineW     = 268;
constexpr int32_t kPaylineY[2]  = {kSymbol - 1, kSymbol * 2 - 1};  // 71〜72, 143〜144
constexpr int32_t kPaylineH     = 2;
constexpr int32_t kStatusY      = kWindowH;             // 216
constexpr int32_t kStatusH      = kScreenH - kWindowH;  // 24

// 色は移植元 miniapp.ts のまま
lv_color_t colorBackground()
{
    return lv_color_hex(0x12121a);
}
lv_color_t colorFrame()
{
    return lv_color_hex(0x5a5a70);
}
lv_color_t colorFrameActive()
{
    return lv_color_hex(0x9a9ab8);
}
lv_color_t colorPayline()
{
    return lv_color_hex(0x8a7a3a);
}
lv_color_t colorText()
{
    return lv_color_hex(0xc8c8d8);
}
lv_color_t colorWin()
{
    return lv_color_hex(0xffcc33);
}

// 枠線・余白・角丸・スクロール・クリックを持たない矩形にする。
void makePlain(Object& o, int32_t x, int32_t y, int32_t w, int32_t h)
{
    o.setPos(x, y);
    o.setSize(w, h);
    o.setRadius(0);
    o.setBorderWidth(0);
    o.setPaddingAll(0);
    o.setShadowWidth(0);
    o.removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    o.removeFlag(LV_OBJ_FLAG_CLICKABLE);
    o.setScrollbarMode(LV_SCROLLBAR_MODE_OFF);
}

enum StatusKind : int {
    kStatusReady    = 0,
    kStatusSpinning = 1,
    kStatusLose     = 2,
    kStatusWinBase  = 10,  // + シンボル番号
};

}  // namespace

View::View(shared::hw::Input& input) : input_(input)
{
    for (int i = 0; i < game::kReelCount; ++i) {
        drawn_pos_[i]   = -1;
        drawn_frame_[i] = -1;
    }
}

View::~View()
{
    end();
}

void View::begin()
{
    LvglLockGuard lock;
    ++screen_id_;

    root_ = std::make_unique<Container>(lv_screen_active());
    makePlain(*root_, 0, 0, kScreenW, kScreenH);
    root_->setBgColor(colorBackground());
    root_->setBgOpa(LV_OPA_COVER);

    // リール: 72x216 の窓 (はみ出しは切る) に、シートを 2 枚縦に 288px ずらして入れる。
    for (int i = 0; i < game::kReelCount; ++i) {
        windows_[i] = std::make_unique<Container>(root_->get());
        makePlain(*windows_[i], kReelX[i], 0, kSymbol, kWindowH);
        windows_[i]->setBgColor(colorBackground());
        windows_[i]->setBgOpa(LV_OPA_COVER);
        // 枠は窓の外側 1px (outline は本体の外に描かれる)
        windows_[i]->setOutlineWidth(1);
        windows_[i]->setOutlineColor(colorFrame());
        lv_obj_set_style_outline_pad(windows_[i]->get(), 0, LV_PART_MAIN);
        for (int k = 0; k < 2; ++k) {
            strips_[i][k] = std::make_unique<Image>(windows_[i]->get());
            strips_[i][k]->setSrc(&rl_reel);
            strips_[i][k]->setPos(0, k * kStrip);
            strips_[i][k]->removeFlag(LV_OBJ_FLAG_CLICKABLE);
        }
        drawn_pos_[i]   = -1;
        drawn_frame_[i] = -1;
    }

    // ペイライン (リールの上に重ねる)
    for (int k = 0; k < 2; ++k) {
        paylines_[k] = std::make_unique<Container>(root_->get());
        makePlain(*paylines_[k], kPaylineX, kPaylineY[k], kPaylineW, kPaylineH);
        paylines_[k]->setBgColor(colorPayline());
        paylines_[k]->setBgOpa(LV_OPA_COVER);
    }
    drawn_payline_win_ = 0;

    // 状態の 1 行
    status_ = std::make_unique<Label>(root_->get());
    status_->setTextFont(&rl_font_jp_20);
    status_->setTextColor(colorText());
    status_->setTextAlign(LV_TEXT_ALIGN_CENTER);
    status_->setWidth(kScreenW);
    status_->setText("");
    const int32_t line_h = lv_font_get_line_height(&rl_font_jp_20);
    status_->setPos(0, kStatusY + (kStatusH - line_h) / 2);
    drawn_status_ = -1;

    // タップ領域: リールの窓と同じ高さで画面を横に 3 等分した透明の領域。
    // 下端の状態の行はタップ領域にしない (ホームインジケータの上スワイプの始点と重ねない)。
    for (int i = 0; i < game::kReelCount; ++i) {
        const int32_t x0 = kScreenW * i / game::kReelCount;
        const int32_t x1 = kScreenW * (i + 1) / game::kReelCount;
        taps_[i]         = std::make_unique<Container>(root_->get());
        makePlain(*taps_[i], x0, 0, x1 - x0, kWindowH);
        taps_[i]->setBgOpa(LV_OPA_TRANSP);
        taps_[i]->addFlag(LV_OBJ_FLAG_CLICKABLE);
        tap_targets_[i] = TapTarget{this, i};
        // 移植元は onTouchBegan (押した瞬間) で止めるので PRESSED で積む。
        taps_[i]->onPressed(&View::onTapPressed, &tap_targets_[i]);
    }
    mclog::tagInfo(kTag, "begin (screen {})", screen_id_);
}

void View::end()
{
    LvglLockGuard lock;
    // 子から先に消してから root を消す (各ラッパが自分のオブジェクトを消す)。
    for (auto& t : taps_) {
        t.reset();
    }
    status_.reset();
    for (auto& p : paylines_) {
        p.reset();
    }
    for (int i = 0; i < game::kReelCount; ++i) {
        strips_[i][0].reset();
        strips_[i][1].reset();
        windows_[i].reset();
    }
    root_.reset();
}

void View::onTapPressed(lv_event_t* e)
{
    // LVGL タスクから呼ばれる (LVGL のロックの中)。キューに積むだけ。
    auto* target = static_cast<TapTarget*>(lv_event_get_user_data(e));
    if (target == nullptr || target->view == nullptr) {
        return;
    }
    target->view->input_.pushButton(target->view->screen_id_, target->index);
}

void View::render(const game::Slot& slot)
{
    LvglLockGuard lock;
    if (!root_) {
        return;
    }

    // リール: 位置 p (0〜287) に対して 1 枚目を y = -p、2 枚目を y = 288 - p。動いたリールだけ。
    for (int i = 0; i < game::kReelCount; ++i) {
        const game::Reel& reel = slot.reel(i);
        const int pos          = static_cast<int>(std::floor(game::normalize(reel.position)));
        if (pos != drawn_pos_[i]) {
            strips_[i][0]->setY(-pos);
            strips_[i][1]->setY(kStrip - pos);
            drawn_pos_[i] = pos;
        }
        const int active = reel.phase == game::Reel::Phase::Stopped ? 0 : 1;
        if (active != drawn_frame_[i]) {
            windows_[i]->setOutlineColor(active ? colorFrameActive() : colorFrame());
            drawn_frame_[i] = active;
        }
    }

    const bool result = slot.phase() == game::Slot::Phase::Result;
    const int win     = result && slot.win() ? 1 : 0;
    if (win != drawn_payline_win_) {
        for (auto& p : paylines_) {
            p->setBgColor(win ? colorWin() : colorPayline());
        }
        drawn_payline_win_ = win;
    }

    int kind = kStatusReady;
    if (slot.phase() == game::Slot::Phase::Spinning) {
        kind = kStatusSpinning;
    } else if (result) {
        kind = win ? kStatusWinBase + slot.winSymbol() : kStatusLose;
    }
    if (kind != drawn_status_) {
        if (kind >= kStatusWinBase) {
            const int symbol = kind - kStatusWinBase;
            char text[64];
            std::snprintf(text, sizeof(text), str::kWin,
                          (symbol >= 0 && symbol < game::kSymbolCount) ? str::kSymbolNames[symbol] : "");
            status_->setText(text);
            status_->setTextColor(colorWin());
        } else {
            status_->setText(kind == kStatusSpinning ? str::kSpinning
                             : kind == kStatusLose   ? str::kLose
                                                     : str::kReady);
            status_->setTextColor(colorText());
        }
        drawn_status_ = kind;
    }
}

}  // namespace roulette::view
