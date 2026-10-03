/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "head.h"

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

stackchan::motion::Motion& motion()
{
    return GetStackChan().motion();
}

}  // namespace

void Head::begin(uint32_t now_ms)
{
    faulted_     = false;
    watching_    = false;
    moving_      = false;
    has_cmd_     = false;
    active_      = true;
    paused_      = false;
    pending_     = false;
    fault_count_ = 0;

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
    if (!active_) {
        return;
    }
    paused_  = false;  // 閉じるときは待っている途中でも正面に戻す
    pending_ = false;
    if (!faulted_) {
        // 低速の 1 指示で正面に戻す。戻りきる前にアプリが閉じても、ランチャーが Motion を進める。
        neutral(now_ms);
    }
    motion().setAutoTorqueReleaseEnabled(true);  // 純正の既定に戻す
    active_   = false;
    watching_ = false;
    mclog::tagInfo(kTag, "end: back to neutral, auto torque release restored");
}

void Head::update(uint32_t now_ms)
{
    if (!active_) {
        return;
    }
    // 純正 Motion は update() でアニメーションを 50 Hz で進める (内部で間引き済み)。
    motion().update();

    // 応答なしの後の待ちが終わったら、待っている間に来た最新の指示を送る。
    if (paused_ && now_ms - paused_since_ms_ >= cfg::HEAD_FAULT_RETRY_MS) {
        paused_ = false;
        mclog::tagInfo(kTag, "resume head commands after pause ({} faults in a row)", fault_count_);
        if (pending_ && !faulted_) {
            pending_ = false;
            command(target_x_, target_y_, now_ms);
        }
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
    moving_       = motion().isMoving();
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
        auto& m        = motion();
        const int cur_x = m.getCurrentYawAngle();
        const int cur_y = m.getCurrentPitchAngle();
        if (std::abs(cur_x - target_x_) <= cfg::HEAD_SETTLE_TOLERANCE &&
            std::abs(cur_y - target_y_) <= cfg::HEAD_SETTLE_TOLERANCE) {
            watching_    = false;
            moving_      = false;
            fault_count_ = 0;  // 目標の近くまでは来ているので、応答なしには数えない
            mclog::tagWarn(kTag, "servo still reports moving {} ms after command, but at ({},{}) near target ({},{}); treat as settled",
                           now_ms - last_cmd_ms_, cur_x, cur_y, target_x_, target_y_);
            return;
        }
        // 目標から離れたまま止まらない。すぐには諦めず、少し待ってから指示をまた受け付ける。
        // 続けて HEAD_FAULT_LIMIT 回なら、そのセッションの間は首を止める (spec §9 固定カメラで続行)。
        watching_ = false;
        moving_   = false;
        ++fault_count_;
        motion().stop();
        // 以後の targetX/Y (edge に「今のサーボ角」として送る) と相対指示は、今の実際の角度を基準にする。
        // ログには元の指令角を出す (診断用)。
        const int cmd_x = target_x_;
        const int cmd_y = target_y_;
        target_x_       = clampX(cur_x);
        target_y_       = clampY(cur_y);
        if (fault_count_ >= cfg::HEAD_FAULT_LIMIT) {
            faulted_ = true;
            pending_ = false;
            mclog::tagError(kTag,
                            "servo not settling {} ms after command (at x={} y={}, target x={} y={}); {} times in a row, head disabled",
                            now_ms - last_cmd_ms_, cur_x, cur_y, cmd_x, cmd_y, fault_count_);
        } else {
            paused_          = true;
            pending_         = false;
            paused_since_ms_ = now_ms;
            mclog::tagWarn(kTag,
                           "servo not settling {} ms after command (at x={} y={}, target x={} y={}); pause {} ms and retry ({}/{})",
                           now_ms - last_cmd_ms_, cur_x, cur_y, cmd_x, cmd_y, cfg::HEAD_FAULT_RETRY_MS,
                           fault_count_, cfg::HEAD_FAULT_LIMIT);
        }
    }
}

void Head::command(int x, int y, uint32_t now_ms)
{
    if (faulted_) {
        // 首を止めた後は目標も変えない (targetX/Y は止まった実際の角度のまま edge に送る)。
        return;
    }
    target_x_     = clampX(x);
    target_y_     = clampY(y);
    last_cmd_ms_  = now_ms;
    last_poll_ms_ = now_ms;
    has_cmd_      = true;
    if (paused_) {
        pending_ = true;  // 待ちが終わったら update() がこの目標を送る
        return;
    }
    moving_         = true;  // 指示直後は動作中とみなす (次の update() で実測に置き換わる)
    watching_       = true;
    last_motion_ms_ = now_ms;
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
    if (paused_) {
        return false;  // 顔追従の小さな指示は、待っている間は捨てる (古くなるので覚えない)
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
