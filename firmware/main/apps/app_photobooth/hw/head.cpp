/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "head.h"

#include <mooncake_log.h>
#include <stackchan/stackchan.h>

#include "../config.h"

namespace photobooth::hw {

namespace {

constexpr const char* kTag = "PB-Head";

namespace cfg = photobooth::config;

stackchan::motion::Motion& motion()
{
    return GetStackChan().motion();
}

}  // namespace

void MotionServo::moveWithSpeed(int x, int y, int speed)
{
    motion().moveWithSpeed(x, y, speed);
}
bool MotionServo::isMoving()
{
    return motion().isMoving();
}
int MotionServo::currentX()
{
    return motion().getCurrentYawAngle();
}
int MotionServo::currentY()
{
    return motion().getCurrentPitchAngle();
}
void MotionServo::stop()
{
    motion().stop();
}

void Head::begin(uint32_t now_ms)
{
    logic_.begin();

    auto& m = motion();
    // 撮影中は止まっても保持する (純正既定のトルク自動解放だと pitch が垂れてフレームがずれる)。
    m.setAutoTorqueReleaseEnabled(false);
    // 指示のたびに現在角からアニメーションを始める (純正の既定値。他アプリが変えていても戻す)。
    m.setAutoAngleSyncEnabled(true);

    mclog::tagInfo(kTag, "begin: current x={} y={}", m.getCurrentYawAngle(), m.getCurrentPitchAngle());
    neutral(now_ms);
}

void Head::end(uint32_t now_ms)
{
    // 待ちを取り消し、異常でなければ neutral を指示する (HeadLogic::end)。
    if (!logic_.end(now_ms)) {
        return;
    }
    if (!logic_.faulted()) {
        mclog::tagInfo(kTag, "moveTo x={} y={}", logic_.targetX(), logic_.targetY());
    }
    motion().setAutoTorqueReleaseEnabled(true);  // 純正の既定に戻す
    mclog::tagInfo(kTag, "end: back to neutral, auto torque release restored");
}

void Head::update(uint32_t now_ms)
{
    if (!logic_.active()) {
        return;
    }
    // 純正 Motion は update() でアニメーションを 50 Hz で進める (内部で間引き済み)。
    motion().update();

    HeadLogic::Detail d;
    const auto event       = logic_.update(now_ms, &d);
    const uint32_t elapsed = now_ms - logic_.lastCommandMs();
    switch (event) {
        case HeadLogic::Event::None:
            break;
        case HeadLogic::Event::Resumed:
            mclog::tagInfo(kTag, "resume head commands after pause ({} faults in a row)", logic_.faultCount());
            break;
        case HeadLogic::Event::Settled:
            mclog::tagInfo(kTag, "settled at target x={} y={} in {} ms", logic_.targetX(), logic_.targetY(), elapsed);
            break;
        case HeadLogic::Event::NearTargetSettled:
            mclog::tagWarn(kTag, "servo still reports moving {} ms after command, but at ({},{}) near target ({},{}); treat as settled",
                           elapsed, d.cur_x, d.cur_y, d.cmd_x, d.cmd_y);
            break;
        case HeadLogic::Event::Faulted:
            mclog::tagError(kTag,
                            "servo not settling {} ms after command (at x={} y={}, target x={} y={}); {} times in a row, head disabled",
                            elapsed, d.cur_x, d.cur_y, d.cmd_x, d.cmd_y, logic_.faultCount());
            break;
        case HeadLogic::Event::Paused:
            mclog::tagWarn(kTag,
                           "servo not settling {} ms after command (at x={} y={}, target x={} y={}); pause {} ms and retry ({}/{})",
                           elapsed, d.cur_x, d.cur_y, d.cmd_x, d.cmd_y, cfg::HEAD_FAULT_RETRY_MS, logic_.faultCount(),
                           cfg::HEAD_FAULT_LIMIT);
            break;
    }
}

void Head::moveTo(int x, int y, uint32_t now_ms)
{
    const int cx = clampHeadX(x);
    const int cy = clampHeadY(y);
    if (cx != x || cy != y) {
        mclog::tagWarn(kTag, "moveTo clamped ({},{}) -> ({},{})", x, y, cx, cy);
    }
    logic_.moveTo(cx, cy, now_ms);
    mclog::tagInfo(kTag, "moveTo x={} y={}", logic_.targetX(), logic_.targetY());
}

bool Head::nudge(int dx, int dy, uint32_t now_ms)
{
    const int prev_x = logic_.targetX();
    const int prev_y = logic_.targetY();
    HeadLogic::Step step;
    if (!logic_.nudge(dx, dy, now_ms, &step)) {
        return false;
    }
    mclog::tagInfo(kTag, "nudge d=({},{}) target ({},{}) -> ({},{})", step.dx, step.dy, prev_x, prev_y,
                   logic_.targetX(), logic_.targetY());
    return true;
}

void Head::neutral(uint32_t now_ms)
{
    moveTo(cfg::HEAD_X_NEUTRAL, cfg::HEAD_Y_NEUTRAL, now_ms);
}

}  // namespace photobooth::hw
