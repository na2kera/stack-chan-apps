/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "lights.h"

#include <hal/hal.h>

namespace roulette::hw {

void Lights::startRainbow(uint32_t now_ms)
{
    now_ms_ = now_ms;
    pattern_.startRainbow(now_ms);
    render(now_ms);
}

void Lights::setReach(bool on)
{
    pattern_.setReach(on);
    render(now_ms_);
}

void Lights::off()
{
    pattern_.off();
    render(now_ms_);
}

void Lights::update(uint32_t now_ms)
{
    now_ms_ = now_ms;
    render(now_ms);
}

void Lights::render(uint32_t now_ms)
{
    Rgb colors[kLedCount];
    if (pattern_.colors(now_ms, colors)) {
        show(colors);
    }
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
