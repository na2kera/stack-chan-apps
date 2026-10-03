/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "head.h"

#include <hal/hal.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>

#include <algorithm>
#include <cstdlib>

#include "../config.h"

namespace photobooth::hw {

namespace {

constexpr const char* kTag = "PB-Head";
// isMoving() は UART でサーボに問い合わせるので間引く。
constexpr uint32_t kPollIntervalMs = 100;

namespace cfg = photobooth::config;

static_assert(cfg::HEAD_X_MIN <= cfg::HEAD_X_NEUTRAL && cfg::HEAD_X_NEUTRAL <= cfg::HEAD_X_MAX,
              "HEAD_X_NEUTRAL must be within [HEAD_X_MIN, HEAD_X_MAX]");
static_assert(cfg::HEAD_Y_MIN <= cfg::HEAD_Y_NEUTRAL && cfg::HEAD_Y_NEUTRAL <= cfg::HEAD_Y_MAX,
              "HEAD_Y_NEUTRAL must be within [HEAD_Y_MIN, HEAD_Y_MAX]");
// 純正の可動域 (hal_servo.cpp: yaw -1280..1280, pitch 30..870) と公式推奨の Y 5〜85° を越えない。
static_assert(cfg::HEAD_X_MIN >= -1280 && cfg::HEAD_X_MAX <= 1280, "HEAD_X out of servo range");
static_assert(cfg::HEAD_Y_MIN >= 50 && cfg::HEAD_Y_MAX <= 850, "HEAD_Y out of 5..85 deg");
static_assert(cfg::HEAD_STEP_MAX > 0, "HEAD_STEP_MAX must be positive");

int clampX(int x)
{
    return std::min(std::max(x, cfg::HEAD_X_MIN), cfg::HEAD_X_MAX);
}
int clampY(int y)
{
    return std::min(std::max(y, cfg::HEAD_Y_MIN), cfg::HEAD_Y_MAX);
}

// 純正 Motion は、HAL のバックグラウンドタスク (_stackchan_update_task) が LVGL のロックを持ったまま
// 50 Hz で update() し、さらに待機中の首振り (IdleMotionModifier) や頭なで (HeadPetModifier) が
// 同じ Motion に指示を出す。こちらから触るときも同じロックを取り、同時に書き換えないようにする。
stackchan::motion::Motion& motion()
{
    return GetStackChan().motion();
}

}  // namespace

uint32_t Head::kRetryMs()
{
    return cfg::HEAD_FAULT_RETRY_MS;
}

void Head::begin(uint32_t now_ms)
{
    faulted_  = false;
    watching_ = false;
    moving_   = false;
    has_cmd_  = false;
    active_   = true;

    paused_      = false;
    fault_count_ = 0;

    LvglLockGuard lock;
    auto& m = motion();
    // 撮影中は純正の自動の首振り (待機中のランダムな首振り、頭なでの反応) を止める。これらは
    // Motion の modify lock を見て何もしなくなる。止めないと首の取り合いになり、こちらの指示が
    // 上書きされて「応答なし」と誤判定されていた (実機ログ: 頭タッチで開始した直後に約 4° ずれた)。
    m.setModifyLock(true);
    // 撮影中は止まっても保持する (純正既定のトルク自動解放だと pitch が垂れてフレームがずれる)。
    m.setAutoTorqueReleaseEnabled(false);
    // 指示のたびに現在角からアニメーションを始める (純正の既定値。他アプリが変えていても戻す)。
    m.setAutoAngleSyncEnabled(true);

    mclog::tagInfo(kTag, "begin: current x={} y={}", m.getCurrentYawAngle(), m.getCurrentPitchAngle());
    neutral(now_ms);
}

void Head::end(uint32_t now_ms)
{
    if (!active_) {
        return;
    }
    paused_ = false;  // 閉じるときは一時停止中でも正面に戻す
    if (!faulted_) {
        // 低速の 1 指示で正面に戻す。戻りきる前にアプリが閉じても、純正のタスクが Motion を進める。
        neutral(now_ms);
    }
    {
        LvglLockGuard lock;
        motion().setAutoTorqueReleaseEnabled(true);  // 純正の既定に戻す
        motion().setModifyLock(false);               // 純正の自動の首振りを戻す
    }
    active_   = false;
    watching_ = false;
    mclog::tagInfo(kTag, "end: back to neutral, auto torque release restored");
}

void Head::update(uint32_t now_ms)
{
    if (!active_) {
        return;
    }
    // Motion の update() は純正のバックグラウンドタスクが 50 Hz で呼んでいるので、ここでは呼ばない
    // (別タスクから同時に進めない)。
    if (paused_ && now_ms - paused_since_ms_ >= cfg::HEAD_FAULT_RETRY_MS) {
        paused_ = false;
        mclog::tagInfo(kTag, "resume head commands after pause ({} faults so far)", fault_count_);
    }

    // 指示してから止まるまでの間だけ問い合わせる。止まった後の一時的な読み取り失敗
    // (ReadMove が -1 を返すと「動作中」に見える) で異常判定しないため。
    if (faulted_ || !watching_) {
        return;
    }
    if (now_ms - last_poll_ms_ < kPollIntervalMs) {
        return;
    }
    last_poll_ms_ = now_ms;
    {
        LvglLockGuard lock;
        // 純正の IMU の反応 (揺すったとき) は終わると modify lock を外すので、ここでかけ直す。
        if (!motion().isModifyLocked()) {
            motion().setModifyLock(true);
        }
        moving_ = motion().isMoving();
    }
    if (moving_) {
        last_motion_ms_ = now_ms;
    }
    if (!moving_) {
        watching_    = false;
        fault_count_ = 0;
        mclog::tagInfo(kTag, "settled at target x={} y={} in {} ms", target_x_, target_y_, now_ms - last_cmd_ms_);
        return;
    }
    if (now_ms - last_cmd_ms_ > cfg::HEAD_MOVE_TIMEOUT_MS) {
        // 目標の近くまで来ているなら、サーボが「動作中」を返し続けているだけとみなして続行する
        // (小さな移動や不感帯での微振動、ReadMove の読み取り失敗で起きる)。
        int cur_x = 0;
        int cur_y = 0;
        {
            LvglLockGuard lock;
            cur_x = motion().getCurrentYawAngle();
            cur_y = motion().getCurrentPitchAngle();
        }
        if (std::abs(cur_x - target_x_) <= cfg::HEAD_SETTLE_TOLERANCE &&
            std::abs(cur_y - target_y_) <= cfg::HEAD_SETTLE_TOLERANCE) {
            watching_ = false;
            moving_   = false;
            mclog::tagWarn(kTag, "servo still reports moving {} ms after command, but at ({},{}) near target ({},{}); treat as settled",
                           now_ms - last_cmd_ms_, cur_x, cur_y, target_x_, target_y_);
            return;
        }
        // 目標から離れたまま止まらない。すぐには諦めず、少し待ってから指示をまた受け付ける。
        // 続けて HEAD_FAULT_LIMIT 回なら、そのセッションの間は首を止める (spec §9 固定カメラで続行)。
        watching_ = false;
        moving_   = false;
        ++fault_count_;
        {
            LvglLockGuard lock;
            motion().stop();
        }
        if (fault_count_ >= cfg::HEAD_FAULT_LIMIT) {
            faulted_ = true;
            mclog::tagError(kTag,
                            "servo not settling {} ms after command (at x={} y={}, target x={} y={}); {} times in a row, head disabled",
                            now_ms - last_cmd_ms_, cur_x, cur_y, target_x_, target_y_, fault_count_);
        } else {
            paused_          = true;
            paused_since_ms_ = now_ms;
            // 次の指示は今の実際の角度から始める (Motion は指示のたびに現在角へ合わせ直す)。
            target_x_ = clampX(cur_x);
            target_y_ = clampY(cur_y);
            mclog::tagWarn(kTag,
                           "servo not settling {} ms after command (at x={} y={}); pause {} ms and retry ({}/{})",
                           now_ms - last_cmd_ms_, cur_x, cur_y, cfg::HEAD_FAULT_RETRY_MS, fault_count_,
                           cfg::HEAD_FAULT_LIMIT);
        }
    }
}

void Head::command(int x, int y, uint32_t now_ms)
{
    target_x_     = clampX(x);
    target_y_     = clampY(y);
    last_cmd_ms_  = now_ms;
    last_poll_ms_ = now_ms;
    has_cmd_      = true;
    if (faulted_ || (paused_ && now_ms - paused_since_ms_ < cfg::HEAD_FAULT_RETRY_MS)) {
        return;
    }
    moving_         = true;  // 指示直後は動作中とみなす (次の update() で実測に置き換わる)
    watching_       = true;
    last_motion_ms_ = now_ms;
    LvglLockGuard lock;
    motion().moveWithSpeed(target_x_, target_y_, cfg::HEAD_SPEED);
}

void Head::moveTo(int x, int y, uint32_t now_ms)
{
    const int cx = clampX(x);
    const int cy = clampY(y);
    if (cx != x || cy != y) {
        mclog::tagWarn(kTag, "moveTo clamped ({},{}) -> ({},{})", x, y, cx, cy);
    }
    command(cx, cy, now_ms);
    mclog::tagInfo(kTag, "moveTo x={} y={}", target_x_, target_y_);
}

bool Head::nudge(int dx, int dy, uint32_t now_ms)
{
    if (has_cmd_ && now_ms - last_cmd_ms_ < cfg::HEAD_STEP_INTERVAL_MS) {
        return false;
    }
    if (paused_ && now_ms - paused_since_ms_ < cfg::HEAD_FAULT_RETRY_MS) {
        return false;  // 応答なしの直後は少し待つ
    }
    dx               = std::min(std::max(dx, -cfg::HEAD_STEP_MAX), cfg::HEAD_STEP_MAX);
    dy               = std::min(std::max(dy, -cfg::HEAD_STEP_MAX), cfg::HEAD_STEP_MAX);
    // 1° 未満の指示は出さない (サーボの不感帯以下で、止まったと報告されないため)。
    if (std::abs(dx) < cfg::HEAD_MIN_STEP) dx = 0;
    if (std::abs(dy) < cfg::HEAD_MIN_STEP) dy = 0;
    if (dx == 0 && dy == 0) {
        return false;
    }
    const int prev_x = target_x_;
    const int prev_y = target_y_;
    command(target_x_ + dx, target_y_ + dy, now_ms);
    mclog::tagInfo(kTag, "nudge d=({},{}) target ({},{}) -> ({},{})", dx, dy, prev_x, prev_y, target_x_, target_y_);
    return true;
}

void Head::neutral(uint32_t now_ms)
{
    moveTo(cfg::HEAD_X_NEUTRAL, cfg::HEAD_Y_NEUTRAL, now_ms);
}

}  // namespace photobooth::hw
