/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 頭部 LED の演出 (docs/design/app-roulette.md §4 hw/lights。移植元 mod.ts の lightRainbow / lightOn の代わり)。
//
// GetHAL().setRgbColor(index, r, g, b) + refreshRgb() で 12 個 (0〜5 が左、6〜11 が右) を光らせる。
// I2C の書き込みを毎フレームしないよう、前回書いた色と違うときだけ書き、虹色の更新は 50 ms ごとにする。
// onRunning のスレッドからだけ呼ぶ。
// どの色を出すか (虹色の時間・リーチ・消灯) は LightPattern (light_pattern.h) が決め、
// Lights はその 12 色を LED に書くだけ。
#pragma once

#include <cstdint>

#include "light_pattern.h"

namespace roulette::hw {

class Lights {
public:
    static constexpr int kLedCount = LightPattern::kLedCount;

    // 開始の演出: 約 1.2 秒間、色相をずらした虹色を回し、その後消灯する。
    void startRainbow(uint32_t now_ms);
    // true: リーチの黄色 (24, 18, 0)。虹色の途中なら打ち切る。false: 消灯。
    void setReach(bool on);
    // 消灯。
    void off();
    // 虹色の更新と、1.2 秒経ったときの消灯。毎 tick 呼ぶ。
    void update(uint32_t now_ms);

private:
    // pattern_ の色が前回と変わっていれば書く。
    void render(uint32_t now_ms);
    // colors を書く。前回と同じ LED は書かず、1 つでも変われば refreshRgb() する。
    void show(const Rgb (&colors)[kLedCount]);

    LightPattern pattern_;
    uint32_t now_ms_ = 0;  // 最後に受けた時刻。消灯・リーチの色は時刻によらないので、setReach() / off() はこれで描く
    Rgb shown_[kLedCount];
    bool shown_valid_ = false;  // shown_ が LED の実際の色と一致しているか (最初の 1 回は必ず書く)
};

}  // namespace roulette::hw
