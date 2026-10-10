/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// スロットの画面 (docs/design/app-roulette.md §4 view, §5)。
//
// lv_screen_active() の上に 1 枚のルートを作り、リール 3 本・ペイライン・状態の 1 行・タップ領域を置く。
// 部品は lvgl_cpp の型どおりに unique_ptr で持つ (Object* 経由で delete しない)。
// どのメソッドも中で LvglLockGuard を取るので、呼び出し側は取らないこと。
// タップ領域が押されると shared::hw::Input に Button (index = リールの列 0, 1, 2) を積む。
#pragma once

#include <smooth_lvgl.hpp>

#include <cstdint>
#include <memory>

#include "../game/slot.h"

namespace shared::hw {
class Input;
}

namespace roulette::view {

class View {
public:
    explicit View(shared::hw::Input& input);
    ~View();
    View(const View&)            = delete;
    View& operator=(const View&) = delete;

    // 画面を作る。
    void begin();
    // 画面を消す。
    void end();

    // 今の画面の世代番号 (Input の Event::screen と比べる)。
    uint32_t screenId() const
    {
        return screen_id_;
    }

    // ゲームの状態を描く。変わったところだけ LVGL に反映する。
    void render(const game::Slot& slot);

private:
    using Container = smooth_ui_toolkit::lvgl_cpp::Container;
    using Image     = smooth_ui_toolkit::lvgl_cpp::Image;
    using Label     = smooth_ui_toolkit::lvgl_cpp::Label;

    // タップ領域の LVGL コールバックに渡す (LVGL タスクから呼ばれる)。
    struct TapTarget {
        View* view = nullptr;
        int index  = 0;
    };
    static void onTapPressed(lv_event_t* e);

    shared::hw::Input& input_;
    uint32_t screen_id_ = 0;

    std::unique_ptr<Container> root_;
    std::unique_ptr<Container> windows_[game::kReelCount];
    std::unique_ptr<Image> strips_[game::kReelCount][2];  // 窓の中のシート 2 枚 (末尾をまたいでも続きが見える)
    std::unique_ptr<Container> paylines_[2];
    std::unique_ptr<Label> status_;
    std::unique_ptr<Container> taps_[game::kReelCount];
    TapTarget tap_targets_[game::kReelCount];

    // 最後に描いた状態 (変わったときだけ反映する)
    int drawn_pos_[game::kReelCount];
    int drawn_frame_[game::kReelCount];  // 1: 回転中の色、0: 停止の色、-1: 未描画
    int drawn_payline_win_ = -1;
    int drawn_status_      = -1;  // 状態の 1 行の種類 (Ready / Spinning / 当たり + シンボル / はずれ)
};

}  // namespace roulette::view
