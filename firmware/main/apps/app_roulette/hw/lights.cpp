/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "lights.h"

#include <hal/hal.h>

namespace roulette::hw {

namespace {

constexpr uint32_t kRainbowMs      = 1200;  // 移植元 START_LIGHT_MS
constexpr uint32_t kRainbowStepMs  = 50;    // 虹色の更新間隔
constexpr uint32_t kRainbowCycleMs = 800;   // 色相が一周する時間
constexpr uint8_t kMaxLevel        = 24;    // 最大成分 (移植元のリーチの黄色 (24, 18, 0) と同程度)
constexpr uint8_t kReachR          = 24;
constexpr uint8_t kReachG          = 18;
constexpr uint8_t kReachB          = 0;

// 色相 (0〜359) を明るさ kMaxLevel の RGB にする (彩度 100%)。
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

}  // namespace

void Lights::startRainbow(uint32_t now_ms)
{
    mode_             = Mode::Rainbow;
    rainbow_since_ms_ = now_ms;
    drawRainbow(now_ms);
}

void Lights::setReach(bool on)
{
    if (!on) {
        off();
        return;
    }
    // 虹色の途中でも打ち切って黄色にする (移植元: リーチの黄色を開始演出のタイマーで消さない)。
    mode_ = Mode::Reach;
    fill(kReachR, kReachG, kReachB);
}

void Lights::off()
{
    mode_ = Mode::Off;
    fill(0, 0, 0);
}

void Lights::update(uint32_t now_ms)
{
    if (mode_ != Mode::Rainbow) {
        return;
    }
    if (now_ms - rainbow_since_ms_ >= kRainbowMs) {
        off();
        return;
    }
    if (now_ms - rainbow_drawn_ms_ >= kRainbowStepMs) {
        drawRainbow(now_ms);
    }
}

void Lights::drawRainbow(uint32_t now_ms)
{
    rainbow_drawn_ms_       = now_ms;
    const uint32_t elapsed  = now_ms - rainbow_since_ms_;
    const uint32_t rotation = elapsed % kRainbowCycleMs * 360 / kRainbowCycleMs;
    Rgb colors[kLedCount];
    for (int i = 0; i < kLedCount; ++i) {
        hueToRgb(rotation + static_cast<uint32_t>(i) * 360 / kLedCount, colors[i].r, colors[i].g, colors[i].b);
    }
    show(colors);
}

void Lights::fill(uint8_t r, uint8_t g, uint8_t b)
{
    Rgb colors[kLedCount];
    for (auto& c : colors) {
        c.r = r;
        c.g = g;
        c.b = b;
    }
    show(colors);
}

void Lights::show(const Rgb (&colors)[kLedCount])
{
    bool changed = false;
    for (int i = 0; i < kLedCount; ++i) {
        const Rgb& c = colors[i];
        Rgb& s       = shown_[i];
        if (shown_valid_ && s.r == c.r && s.g == c.g && s.b == c.b) {
            continue;
        }
        GetHAL().setRgbColor(static_cast<uint8_t>(i), c.r, c.g, c.b);
        s       = c;
        changed = true;
    }
    shown_valid_ = true;
    if (changed) {
        GetHAL().refreshRgb();
    }
}

}  // namespace roulette::hw
