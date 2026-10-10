/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "light_pattern.h"

namespace roulette::hw {

namespace {

constexpr uint32_t kRainbowMs      = 1200;  // 移植元 START_LIGHT_MS
constexpr uint32_t kRainbowStepMs  = 50;    // 虹色の更新間隔
constexpr uint32_t kRainbowCycleMs = 800;   // 色相が一周する時間
constexpr uint8_t kReachR          = 24;
constexpr uint8_t kReachG          = 18;
constexpr uint8_t kReachB          = 0;

bool sameColor(const Rgb& a, const Rgb& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

}  // namespace

void hueToRgb(uint32_t hue, uint8_t& r, uint8_t& g, uint8_t& b)
{
    hue                = hue % 360;
    const uint32_t seg = hue / 60;
    const uint32_t f   = hue % 60;
    const auto up      = static_cast<uint8_t>(kMaxLevel * f / 60);
    const auto down    = static_cast<uint8_t>(kMaxLevel - up);
    switch (seg) {
        case 0: r = kMaxLevel, g = up, b = 0; break;
        case 1: r = down, g = kMaxLevel, b = 0; break;
        case 2: r = 0, g = kMaxLevel, b = up; break;
        case 3: r = 0, g = down, b = kMaxLevel; break;
        case 4: r = up, g = 0, b = kMaxLevel; break;
        default: r = kMaxLevel, g = 0, b = down; break;
    }
}

void LightPattern::startRainbow(uint32_t now_ms)
{
    mode_             = Mode::Rainbow;
    rainbow_since_ms_ = now_ms;
    drawRainbow(now_ms);
}

void LightPattern::setReach(bool on)
{
    if (!on) {
        off();
        return;
    }
    // 虹色の途中でも打ち切って黄色にする (移植元: リーチの黄色を開始演出のタイマーで消さない)。
    mode_ = Mode::Reach;
    fill(kReachR, kReachG, kReachB);
}

void LightPattern::off()
{
    mode_ = Mode::Off;
    fill(0, 0, 0);
}

bool LightPattern::colors(uint32_t now_ms, Rgb (&out)[kLedCount])
{
    if (mode_ == Mode::Rainbow) {
        if (now_ms - rainbow_since_ms_ >= kRainbowMs) {
            off();
        } else if (now_ms - rainbow_drawn_ms_ >= kRainbowStepMs) {
            drawRainbow(now_ms);
        }
    }
    bool changed = !emitted_valid_;
    for (int i = 0; i < kLedCount; ++i) {
        if (!sameColor(emitted_[i], current_[i])) {
            changed = true;
        }
        emitted_[i] = current_[i];
        out[i]      = current_[i];
    }
    emitted_valid_ = true;
    return changed;
}

void LightPattern::drawRainbow(uint32_t now_ms)
{
    rainbow_drawn_ms_       = now_ms;
    const uint32_t elapsed  = now_ms - rainbow_since_ms_;
    const uint32_t rotation = elapsed % kRainbowCycleMs * 360 / kRainbowCycleMs;
    for (int i = 0; i < kLedCount; ++i) {
        hueToRgb(rotation + static_cast<uint32_t>(i) * 360 / kLedCount, current_[i].r, current_[i].g, current_[i].b);
    }
}

void LightPattern::fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (auto& c : current_) {
        c.r = r;
        c.g = g;
        c.b = b;
    }
}

}  // namespace roulette::hw
