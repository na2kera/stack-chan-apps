/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "head_logic.h"

#include <algorithm>
#include <cstdlib>

#include "../config.h"

namespace photobooth::hw {

namespace {

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

}  // namespace

int clampHeadX(int x)
{
    return std::min(std::max(x, cfg::HEAD_X_MIN), cfg::HEAD_X_MAX);
}
int clampHeadY(int y)
{
    return std::min(std::max(y, cfg::HEAD_Y_MIN), cfg::HEAD_Y_MAX);
}

void HeadLogic::begin()
{
    faulted_     = false;
    watching_    = false;
    moving_      = false;
    has_cmd_     = false;
    active_      = true;
    paused_      = false;
    pending_     = false;
    fault_count_ = 0;
}

bool HeadLogic::end(uint32_t now_ms)
{
    if (!active_) {
        return false;
    }
    paused_  = false;  // 閉じるときは待っている途中でも正面に戻す
    pending_ = false;
    if (!faulted_) {
        // 低速の 1 指示で正面に戻す。戻りきる前にアプリが閉じても、ランチャーが Motion を進める。
        neutral(now_ms);
    }
    active_   = false;
    watching_ = false;
    return true;
}

HeadLogic::Event HeadLogic::update(uint32_t now_ms, Detail* detail)
{
    if (!active_) {
        return Event::None;
    }
    Event event = Event::None;

    // 応答なしの後の待ちが終わったら、待っている間に来た最新の指示を送る。
    // (送った直後は last_poll_ms_ = now_ms なので、同じ呼び出しで下の判定には進まない。
    //  送る指示が無ければ watching_ は false。どちらでも Resumed だけを返す)
    if (paused_ && now_ms - paused_since_ms_ >= cfg::HEAD_FAULT_RETRY_MS) {
        paused_ = false;
        event   = Event::Resumed;
        if (pending_ && !faulted_) {
            pending_ = false;
            command(target_x_, target_y_, now_ms);
        }
    }

    // 指示してから止まるまでの間だけ問い合わせる。止まった後の一時的な読み取り失敗
    // (ReadMove が -1 を返すと「動作中」に見える) で異常判定しないため。
    if (faulted_ || !watching_) {
        return event;
    }
    if (now_ms - last_poll_ms_ < kPollIntervalMs) {
        return event;
    }
    last_poll_ms_ = now_ms;
    moving_       = servo_.isMoving();
    if (moving_) {
        last_motion_ms_ = now_ms;
    }
    if (!moving_) {
        watching_    = false;
        fault_count_ = 0;
        return Event::Settled;
    }
    if (now_ms - last_cmd_ms_ > cfg::HEAD_MOVE_TIMEOUT_MS) {
        // 目標の近くまで来ているなら、サーボが「動作中」を返し続けているだけとみなして続行する
        // (小さな移動や不感帯での微振動、ReadMove の読み取り失敗で起きる)。
        const int cur_x = servo_.currentX();
        const int cur_y = servo_.currentY();
        if (detail != nullptr) {
            *detail = Detail{cur_x, cur_y, target_x_, target_y_};
        }
        if (std::abs(cur_x - target_x_) <= cfg::HEAD_SETTLE_TOLERANCE &&
            std::abs(cur_y - target_y_) <= cfg::HEAD_SETTLE_TOLERANCE) {
            watching_    = false;
            moving_      = false;
            fault_count_ = 0;  // 目標の近くまでは来ているので、応答なしには数えない
            return Event::NearTargetSettled;
        }
        // 目標から離れたまま止まらない。すぐには諦めず、少し待ってから指示をまた受け付ける。
        // 続けて HEAD_FAULT_LIMIT 回なら、そのセッションの間は首を止める (spec §9 固定カメラで続行)。
        watching_ = false;
        moving_   = false;
        ++fault_count_;
        servo_.stop();
        // 以後の targetX/Y (edge に「今のサーボ角」として送る) と相対指示は、今の実際の角度を基準にする。
        // ログには元の指令角を出す (診断用。detail->cmd_x/cmd_y)。
        target_x_ = clampHeadX(cur_x);
        target_y_ = clampHeadY(cur_y);
        if (fault_count_ >= cfg::HEAD_FAULT_LIMIT) {
            faulted_ = true;
            pending_ = false;
            return Event::Faulted;
        }
        paused_          = true;
        pending_         = false;
        paused_since_ms_ = now_ms;
        return Event::Paused;
    }
    return event;
}

void HeadLogic::command(int x, int y, uint32_t now_ms)
{
    if (faulted_) {
        // 首を止めた後は目標も変えない (targetX/Y は止まった実際の角度のまま edge に送る)。
        return;
    }
    target_x_     = clampHeadX(x);
    target_y_     = clampHeadY(y);
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
    servo_.moveWithSpeed(target_x_, target_y_, cfg::HEAD_SPEED);
}

void HeadLogic::moveTo(int x, int y, uint32_t now_ms)
{
    command(clampHeadX(x), clampHeadY(y), now_ms);
}

bool HeadLogic::nudge(int dx, int dy, uint32_t now_ms, Step* applied)
{
    if (has_cmd_ && now_ms - last_cmd_ms_ < cfg::HEAD_STEP_INTERVAL_MS) {
        return false;
    }
    if (paused_) {
        return false;  // 顔追従の小さな指示は、待っている間は捨てる (古くなるので覚えない)
    }
    dx = std::min(std::max(dx, -cfg::HEAD_STEP_MAX), cfg::HEAD_STEP_MAX);
    dy = std::min(std::max(dy, -cfg::HEAD_STEP_MAX), cfg::HEAD_STEP_MAX);
    // 1° 未満の指示は出さない (サーボの不感帯以下で、止まったと報告されないため)。
    if (std::abs(dx) < cfg::HEAD_MIN_STEP) dx = 0;
    if (std::abs(dy) < cfg::HEAD_MIN_STEP) dy = 0;
    if (dx == 0 && dy == 0) {
        return false;
    }
    if (applied != nullptr) {
        *applied = Step{dx, dy};
    }
    command(target_x_ + dx, target_y_ + dy, now_ms);
    return true;
}

void HeadLogic::neutral(uint32_t now_ms)
{
    moveTo(cfg::HEAD_X_NEUTRAL, cfg::HEAD_Y_NEUTRAL, now_ms);
}

}  // namespace photobooth::hw
