/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "slot.h"

#include <algorithm>
#include <cmath>

namespace roulette::game {

float normalize(float position)
{
    const float wrapped = std::fmod(position, kStripHeight);
    return wrapped < 0.0f ? wrapped + kStripHeight : wrapped;
}

int paylineSymbol(float position)
{
    // 移植元は Math.round。正の値なので lround (0.5 は遠い方へ) と同じ結果になる。
    const long row = std::lround(normalize(position) / kSymbolSize);
    return static_cast<int>((row + 1) % kSymbolCount);
}

void Reel::start(int symbol)
{
    position = static_cast<float>(symbol) * kSymbolSize;
    phase    = Phase::Spinning;
    target   = 0.0f;
}

bool Reel::requestStop()
{
    if (phase != Phase::Spinning) {
        return false;
    }
    target = std::ceil((position + kStopSlip) / kSymbolSize) * kSymbolSize;
    phase  = Phase::Stopping;
    return true;
}

void Reel::update()
{
    if (phase == Phase::Spinning) {
        position = normalize(position + kSpinSpeed);
        return;
    }
    if (phase != Phase::Stopping) {
        return;
    }
    const float remaining = target - position;
    const float step      = std::max(kStopMinSpeed, std::min(kSpinSpeed, remaining * kStopEasing));
    if (remaining <= step) {
        position = normalize(target);
        phase    = Phase::Stopped;
        return;
    }
    position += step;
}

Slot::Slot()
{
    for (int i = 0; i < kReelCount; ++i) {
        reels_[i].position = static_cast<float>(i) * kSymbolSize;
    }
}

int Slot::winSymbol() const
{
    if (phase_ != Phase::Result || !win_) {
        return -1;
    }
    return paylineSymbol(reels_[0].position);
}

void Slot::start(int r0, int r1, int r2)
{
    const int symbols[kReelCount] = {r0, r1, r2};
    for (int i = 0; i < kReelCount; ++i) {
        const int s = ((symbols[i] % kSymbolCount) + kSymbolCount) % kSymbolCount;
        reels_[i].start(s);
    }
    phase_ = Phase::Spinning;
    win_   = false;
    reach_ = false;
}

bool Slot::stopReel(int index)
{
    if (phase_ != Phase::Spinning || index < 0 || index >= kReelCount) {
        return false;
    }
    return reels_[index].requestStop();
}

void Slot::step()
{
    for (auto& reel : reels_) {
        reel.update();
    }
    if (phase_ != Phase::Spinning) {
        return;
    }
    int stopped[kReelCount];
    int count = 0;
    for (int i = 0; i < kReelCount; ++i) {
        if (reels_[i].phase == Reel::Phase::Stopped) {
            stopped[count++] = i;
        }
    }
    if (count == kReelCount) {
        // 全停止。同じフレームで 3 つ目が止まった場合もリーチにはせず結果へ (移植元と同じ)。
        reach_          = false;
        const int first = paylineSymbol(reels_[0].position);
        win_            = true;
        for (int i = 1; i < kReelCount; ++i) {
            if (paylineSymbol(reels_[i].position) != first) {
                win_ = false;
            }
        }
        phase_ = Phase::Result;
        return;
    }
    reach_ = count == 2 &&
             paylineSymbol(reels_[stopped[0]].position) == paylineSymbol(reels_[stopped[1]].position);
}

int Slot::advance(uint32_t now_ms)
{
    if (!has_time_ || phase_ != Phase::Spinning) {
        last_ms_  = now_ms;
        has_time_ = true;
        return 0;
    }
    const uint32_t elapsed = now_ms - last_ms_;  // millis の一周 (約 49 日) もこの引き算で扱える
    uint32_t n             = elapsed / kFrameMs;
    if (n > static_cast<uint32_t>(kMaxStepsPerAdvance)) {
        // 長く止まっていた。上限だけ進めて、残りの遅れは捨てる。
        n        = kMaxStepsPerAdvance;
        last_ms_ = now_ms;
    } else {
        last_ms_ += n * kFrameMs;
    }
    int steps = 0;
    for (uint32_t i = 0; i < n && phase_ == Phase::Spinning; ++i) {
        step();
        ++steps;
    }
    return steps;
}

}  // namespace roulette::game
