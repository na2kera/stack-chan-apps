/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 頭部 LED の色の計算 (docs/design/testable-logic-step1.md §5 E)。hw/lights.cpp の Lights から切り出した。
// 開始の虹色 (1.2 秒で消灯、50 ms ごとに更新)・リーチの黄色・消灯のどれを出すかと、12 個の色を決める。
// 時刻は引数で受け、LED には書かない (HAL に依存しない。firmware/tests/light_pattern_test.cpp でホストでテストする)。
// 書き込みは Lights が colors() の結果を見て行う。
#pragma once

#include <cstdint>

namespace roulette::hw {

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline constexpr uint8_t kMaxLevel = 24;  // 最大成分 (移植元のリーチの黄色 (24, 18, 0) と同程度)

// 色相 (0〜359。それ以上は 360 で割った余り) を明るさ kMaxLevel の RGB にする (彩度 100%)。
void hueToRgb(uint32_t hue, uint8_t& r, uint8_t& g, uint8_t& b);

class LightPattern {
public:
    static constexpr int kLedCount = 12;  // 0〜5 が左、6〜11 が右

    // 開始の演出: 約 1.2 秒間、色相をずらした虹色を回し、その後消灯する。
    void startRainbow(uint32_t now_ms);
    // true: リーチの黄色 (24, 18, 0)。虹色の途中なら打ち切る。false: 消灯。
    void setReach(bool on);
    // 消灯。
    void off();

    // now_ms の 12 個の色を out に入れる。虹色の 1.2 秒経過での消灯と 50 ms ごとの更新もここで行う
    // (消灯・リーチの色は時刻によらない)。前回の colors() と出力が変わったら (最初の 1 回は必ず) true。
    bool colors(uint32_t now_ms, Rgb (&out)[kLedCount]);

private:
    enum class Mode : uint8_t { Off, Rainbow, Reach };

    void drawRainbow(uint32_t now_ms);
    void fill(uint8_t r, uint8_t g, uint8_t b);

    Mode mode_                 = Mode::Off;
    uint32_t rainbow_since_ms_ = 0;
    uint32_t rainbow_drawn_ms_ = 0;
    Rgb current_[kLedCount];    // 今出すべき色
    Rgb emitted_[kLedCount];    // 前回 colors() で返した色
    bool emitted_valid_ = false;  // colors() を 1 回でも呼んだか
};

}  // namespace roulette::hw
