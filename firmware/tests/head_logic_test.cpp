/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の hw/head_logic (首の応答監視。docs/design/testable-logic-step1.md §5 D) をホストで確かめる。
// 純正 Motion の代わりに、呼ばれた指示を覚えて決めた値を返す偽の Servo を使う。
#include <apps/app_photobooth/config.h>
#include <apps/app_photobooth/hw/head_logic.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

namespace cfg = photobooth::config;
using photobooth::hw::clampHeadX;
using photobooth::hw::clampHeadY;
using photobooth::hw::HeadLogic;
using photobooth::hw::Servo;
using Event = HeadLogic::Event;

// update() の問い合わせ間隔 (head_logic.cpp の kPollIntervalMs と同じ)。
constexpr uint32_t kPoll = 100;

void fail(const char* label)
{
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
}

void expectEqual(long long actual, long long expected, const char* label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void expectTrue(bool value, const char* label)
{
    if (!value) {
        fail(label);
    }
}

void expectEvent(Event actual, Event expected, const char* label)
{
    expectEqual(static_cast<int>(actual), static_cast<int>(expected), label);
}

struct FakeServo : Servo {
    struct Move {
        int x;
        int y;
        int speed;
    };
    std::vector<Move> moves;
    int stops       = 0;
    int polls       = 0;
    bool moving     = true;  // isMoving() が返す値
    int cur_x       = 0;     // currentX() / currentY() が返す値
    int cur_y       = 0;

    void moveWithSpeed(int x, int y, int speed) override
    {
        moves.push_back({x, y, speed});
    }
    bool isMoving() override
    {
        ++polls;
        return moving;
    }
    int currentX() override
    {
        return cur_x;
    }
    int currentY() override
    {
        return cur_y;
    }
    void stop() override
    {
        ++stops;
    }
};

// begin() + neutral() (Head::begin と同じ) を t0 に行う。
void start(HeadLogic& head, uint32_t t0)
{
    head.begin();
    head.neutral(t0);
}

// 指示から timeout を過ぎて目標から離れたまま → Paused (または Faulted) にする。now を返す。
Event driveToFault(HeadLogic& head, FakeServo& servo, uint32_t cmd_ms, uint32_t& now)
{
    servo.moving = true;
    servo.cur_x  = cfg::HEAD_X_MAX;  // 目標 (neutral 付近) から離れている
    servo.cur_y  = cfg::HEAD_Y_MIN;
    now          = cmd_ms + cfg::HEAD_MOVE_TIMEOUT_MS + 1;
    return head.update(now);
}

void testNeutralAndMoveTo()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    expectTrue(head.active(), "active after begin");
    expectEqual(static_cast<long long>(servo.moves.size()), 1, "neutral sends one command");
    expectEqual(servo.moves[0].x, cfg::HEAD_X_NEUTRAL, "neutral x");
    expectEqual(servo.moves[0].y, cfg::HEAD_Y_NEUTRAL, "neutral y");
    expectEqual(servo.moves[0].speed, cfg::HEAD_SPEED, "speed is HEAD_SPEED");
    expectTrue(head.isMoving(), "moving right after command");

    head.moveTo(10000, -10000, 2000);
    expectEqual(servo.moves.back().x, cfg::HEAD_X_MAX, "moveTo clamps x to max");
    expectEqual(servo.moves.back().y, cfg::HEAD_Y_MIN, "moveTo clamps y to min");
    expectEqual(head.targetX(), cfg::HEAD_X_MAX, "target x clamped");
    head.moveTo(-10000, 10000, 2100);
    expectEqual(head.targetX(), cfg::HEAD_X_MIN, "moveTo clamps x to min");
    expectEqual(head.targetY(), cfg::HEAD_Y_MAX, "moveTo clamps y to max");

    expectEqual(clampHeadX(cfg::HEAD_X_MIN - 1), cfg::HEAD_X_MIN, "clampHeadX below");
    expectEqual(clampHeadX(0), 0, "clampHeadX inside");
    expectEqual(clampHeadY(cfg::HEAD_Y_MAX + 1), cfg::HEAD_Y_MAX, "clampHeadY above");
}

void testNudge()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    const size_t n0 = servo.moves.size();

    // 間隔 HEAD_STEP_INTERVAL_MS 未満は false
    expectTrue(!head.nudge(20, 0, 1000 + cfg::HEAD_STEP_INTERVAL_MS - 1), "nudge too soon");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n0), "too soon sends nothing");

    // ±HEAD_STEP_MAX に制限
    HeadLogic::Step step;
    uint32_t t = 1000 + cfg::HEAD_STEP_INTERVAL_MS;
    expectTrue(head.nudge(1000, -1000, t, &step), "nudge at interval");
    expectEqual(step.dx, cfg::HEAD_STEP_MAX, "step dx limited");
    expectEqual(step.dy, -cfg::HEAD_STEP_MAX, "step dy limited");
    expectEqual(head.targetX(), cfg::HEAD_X_NEUTRAL + cfg::HEAD_STEP_MAX, "target x after nudge");
    expectEqual(head.targetY(), cfg::HEAD_Y_NEUTRAL - cfg::HEAD_STEP_MAX, "target y after nudge");
    expectEqual(servo.moves.back().x, head.targetX(), "nudge sends target x");

    // HEAD_MIN_STEP 未満は 0 扱い。両方 0 なら false
    t += cfg::HEAD_STEP_INTERVAL_MS;
    expectTrue(!head.nudge(cfg::HEAD_MIN_STEP - 1, -(cfg::HEAD_MIN_STEP - 1), t), "nudge below min step");
    expectTrue(head.nudge(cfg::HEAD_MIN_STEP - 1, cfg::HEAD_MIN_STEP, t, &step), "nudge with one axis");
    expectEqual(step.dx, 0, "small dx dropped");
    expectEqual(step.dy, cfg::HEAD_MIN_STEP, "dy kept at min step");

    // 可動域でクランプ
    for (int i = 0; i < 40; ++i) {
        t += cfg::HEAD_STEP_INTERVAL_MS;
        head.nudge(cfg::HEAD_STEP_MAX, cfg::HEAD_STEP_MAX, t);
    }
    expectEqual(head.targetX(), cfg::HEAD_X_MAX, "nudge clamps at x max");
    expectEqual(head.targetY(), cfg::HEAD_Y_MAX, "nudge clamps at y max");
    for (int i = 0; i < 40; ++i) {
        t += cfg::HEAD_STEP_INTERVAL_MS;
        head.nudge(-cfg::HEAD_STEP_MAX, -cfg::HEAD_STEP_MAX, t);
    }
    expectEqual(head.targetX(), cfg::HEAD_X_MIN, "nudge clamps at x min");
    expectEqual(head.targetY(), cfg::HEAD_Y_MIN, "nudge clamps at y min");

    // begin() 直後 (まだ指示が無い) なら間隔を見ない
    FakeServo servo2;
    HeadLogic head2(servo2);
    head2.begin();
    expectTrue(head2.nudge(20, 0, 0), "first nudge without prior command");
}

void testSettled()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);

    // 問い合わせは kPoll ごと
    servo.moving = true;
    expectEvent(head.update(1000 + kPoll - 1), Event::None, "no poll before interval");
    expectEqual(servo.polls, 0, "isMoving not asked before interval");
    expectEvent(head.update(1000 + kPoll), Event::None, "still moving");
    expectEqual(servo.polls, 1, "isMoving asked once");
    expectTrue(head.isMoving(), "isMoving cached true");

    servo.moving = false;
    expectEvent(head.update(1000 + 2 * kPoll), Event::Settled, "settled");
    expectTrue(!head.isMoving(), "not moving after settled");
    // 止まった後は問い合わせない
    expectEvent(head.update(1000 + 10 * kPoll), Event::None, "no event after settled");
    expectEqual(servo.polls, 2, "no poll after settled");
}

void testNearTargetSettled()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    servo.moving = true;
    servo.cur_x  = cfg::HEAD_X_NEUTRAL + cfg::HEAD_SETTLE_TOLERANCE;
    servo.cur_y  = cfg::HEAD_Y_NEUTRAL - cfg::HEAD_SETTLE_TOLERANCE;

    // timeout ちょうどはまだ (> で判定)
    expectEvent(head.update(1000 + cfg::HEAD_MOVE_TIMEOUT_MS), Event::None, "not yet at exact timeout");
    HeadLogic::Detail d;
    expectEvent(head.update(1000 + cfg::HEAD_MOVE_TIMEOUT_MS + kPoll, &d), Event::NearTargetSettled,
                "near target after timeout");
    expectEqual(servo.stops, 0, "near target does not stop");
    expectEqual(d.cur_x, servo.cur_x, "detail cur_x");
    expectEqual(d.cmd_x, cfg::HEAD_X_NEUTRAL, "detail cmd_x");
    expectEqual(head.targetX(), cfg::HEAD_X_NEUTRAL, "target unchanged");
    expectTrue(!head.isMoving(), "not moving after near target");
    expectEqual(head.faultCount(), 0, "near target is not a fault");
}

void testPauseAndResume()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);

    uint32_t now = 0;
    HeadLogic::Detail d;
    servo.cur_x = cfg::HEAD_X_MAX + 100;  // 可動域の外 → 目標はクランプした値に置き換わる
    servo.cur_y = cfg::HEAD_Y_MIN;
    now         = 1000 + cfg::HEAD_MOVE_TIMEOUT_MS + 1;
    expectEvent(head.update(now, &d), Event::Paused, "far from target -> paused");
    expectEqual(servo.stops, 1, "paused calls stop()");
    expectEqual(head.faultCount(), 1, "fault count 1");
    expectEqual(d.cmd_x, cfg::HEAD_X_NEUTRAL, "detail keeps commanded x");
    expectEqual(d.cur_x, cfg::HEAD_X_MAX + 100, "detail has raw current x");
    expectEqual(head.targetX(), cfg::HEAD_X_MAX, "target replaced by clamped current x");
    expectEqual(head.targetY(), cfg::HEAD_Y_MIN, "target replaced by current y");
    expectTrue(!head.isMoving(), "not moving while paused");
    expectTrue(!head.faulted(), "not faulted after one");

    // 待っている間の nudge は false (覚えない)、moveTo は保留
    const size_t n = servo.moves.size();
    expectTrue(!head.nudge(cfg::HEAD_STEP_MAX, 0, now + cfg::HEAD_STEP_INTERVAL_MS), "nudge while paused");
    head.moveTo(100, 400, now + 10);
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n), "moveTo held while paused");
    expectEqual(head.targetX(), 100, "held target x");
    // 応答なしと判定した問い合わせでも isMoving() は true なので、lastMotionMs はその時刻。保留の指示では変わらない
    expectEqual(head.lastMotionMs(), now, "held command does not touch lastMotionMs");

    // HEAD_FAULT_RETRY_MS 後に Resumed で送る
    expectEvent(head.update(now + cfg::HEAD_FAULT_RETRY_MS - 1), Event::None, "still paused");
    const uint32_t resume = now + cfg::HEAD_FAULT_RETRY_MS;
    expectEvent(head.update(resume), Event::Resumed, "resumed");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n + 1), "held command sent");
    expectEqual(servo.moves.back().x, 100, "held command x");
    expectEqual(servo.moves.back().y, 400, "held command y");
    expectTrue(head.isMoving(), "moving after resume");
    expectEqual(head.lastMotionMs(), resume, "lastMotionMs at resume command");

    // 止まったら fault_count は 0 に戻る
    servo.moving = false;
    expectEvent(head.update(resume + kPoll), Event::Settled, "settled after resume");
    expectEqual(head.faultCount(), 0, "fault count reset on settle");
}

void testResumeWithoutPending()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    uint32_t now = 0;
    expectEvent(driveToFault(head, servo, 1000, now), Event::Paused, "paused");
    const size_t n = servo.moves.size();
    expectEvent(head.update(now + cfg::HEAD_FAULT_RETRY_MS), Event::Resumed, "resumed without pending");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n), "nothing to send");
    expectEvent(head.update(now + cfg::HEAD_FAULT_RETRY_MS + 10 * kPoll), Event::None, "no watching after resume");
    // 再開後の nudge は通る
    expectTrue(head.nudge(cfg::HEAD_STEP_MAX, 0, now + cfg::HEAD_FAULT_RETRY_MS + 1), "nudge after resume");
}

void testFaultLimit()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    uint32_t cmd = 1000;
    uint32_t now = 0;
    for (int i = 1; i < cfg::HEAD_FAULT_LIMIT; ++i) {
        expectEvent(driveToFault(head, servo, cmd, now), Event::Paused, "paused before limit");
        expectEqual(head.faultCount(), i, "fault count grows");
        head.update(now + cfg::HEAD_FAULT_RETRY_MS);  // Resumed
        cmd = now + cfg::HEAD_FAULT_RETRY_MS;
        head.moveTo(0, 450, cmd);
    }
    HeadLogic::Detail d;
    servo.cur_x = cfg::HEAD_X_MAX;
    now         = cmd + cfg::HEAD_MOVE_TIMEOUT_MS + 1;
    expectEvent(head.update(now, &d), Event::Faulted, "faulted at limit");
    expectEqual(head.faultCount(), cfg::HEAD_FAULT_LIMIT, "fault count at limit");
    expectEqual(servo.stops, cfg::HEAD_FAULT_LIMIT, "stop() each time");
    expectTrue(head.faulted(), "faulted");
    expectTrue(!head.isMoving(), "isMoving false when faulted");

    // 以後は何も指示しない (目標も変えない)
    const size_t n   = servo.moves.size();
    const int tx     = head.targetX();
    head.moveTo(-100, 300, now + 10);
    // nudge は (切り出す前と同じく) true を返すが、command() が何もしないので指示は出ない
    expectTrue(head.nudge(cfg::HEAD_STEP_MAX, 0, now + 10000), "nudge after fault returns true as before");
    head.neutral(now + 20000);
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n), "no command after fault");
    expectEqual(head.targetX(), tx, "target unchanged after fault");
    servo.moving = true;
    expectEvent(head.update(now + 30000), Event::None, "no polling after fault");
    expectTrue(!head.isMoving(), "still not moving after fault");

    // end() は neutral を送らない
    expectTrue(head.end(now + 40000), "end while active");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n), "end does not move when faulted");
    expectTrue(!head.active(), "inactive after end");
}

void testFaultCountResetsBetweenFaults()
{
    // 応答なしの後に止まれば、続けて数えない。
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    uint32_t now = 0;
    for (int round = 0; round < cfg::HEAD_FAULT_LIMIT + 1; ++round) {
        const uint32_t cmd = round == 0 ? 1000 : now;
        expectEvent(driveToFault(head, servo, cmd, now), Event::Paused, "paused (not faulted) each round");
        now += cfg::HEAD_FAULT_RETRY_MS;
        head.update(now);  // Resumed
        head.moveTo(0, 450, now);
        servo.moving = false;
        now += kPoll;
        expectEvent(head.update(now), Event::Settled, "settled between faults");
        expectEqual(head.faultCount(), 0, "count reset");
        head.moveTo(0, 450, now);
    }
    expectTrue(!head.faulted(), "never faulted");
}

void testEnd()
{
    FakeServo servo;
    HeadLogic head(servo);
    expectTrue(!head.end(0), "end before begin does nothing");
    expectEqual(static_cast<long long>(servo.moves.size()), 0, "no command before begin");

    start(head, 1000);
    uint32_t now = 0;
    driveToFault(head, servo, 1000, now);  // Paused
    head.moveTo(100, 400, now + 1);        // 保留
    const size_t n = servo.moves.size();
    expectTrue(head.end(now + 2), "end while paused");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n + 1),
                "end sends neutral even while paused");
    expectEqual(servo.moves.back().x, cfg::HEAD_X_NEUTRAL, "end neutral x");
    expectEqual(servo.moves.back().y, cfg::HEAD_Y_NEUTRAL, "end neutral y");
    expectTrue(!head.active(), "inactive after end");
    // end の後は update で何もしない (保留も送らない)
    servo.moving = false;
    expectEvent(head.update(now + 100000), Event::None, "update after end");
    expectEqual(static_cast<long long>(servo.moves.size()), static_cast<long long>(n + 1), "nothing after end");
    expectTrue(!head.end(now + 100001), "second end does nothing");
}

void testLastMotionMs()
{
    FakeServo servo;
    HeadLogic head(servo);
    start(head, 1000);
    expectEqual(head.lastMotionMs(), 1000, "lastMotionMs at command");
    servo.moving = true;
    head.update(1000 + kPoll);
    expectEqual(head.lastMotionMs(), 1000 + kPoll, "lastMotionMs at moving poll");
    head.update(1000 + kPoll + 50);  // 問い合わせない
    expectEqual(head.lastMotionMs(), 1000 + kPoll, "no update without poll");
    head.update(1000 + 2 * kPoll);
    expectEqual(head.lastMotionMs(), 1000 + 2 * kPoll, "lastMotionMs at second moving poll");
    servo.moving = false;
    head.update(1000 + 3 * kPoll);
    expectEqual(head.lastMotionMs(), 1000 + 2 * kPoll, "settled poll does not update lastMotionMs");
    head.moveTo(0, 500, 5000);
    expectEqual(head.lastMotionMs(), 5000, "lastMotionMs at next command");
}

void testMillisWrap()
{
    // 指示から millis が一周しても、経過時間で判定する。
    FakeServo servo;
    HeadLogic head(servo);
    const uint32_t t0 = 0xFFFFFFFFu - 1000;
    start(head, t0);
    servo.moving = true;
    servo.cur_x  = cfg::HEAD_X_MAX;
    servo.cur_y  = cfg::HEAD_Y_MIN;
    // 一周した直後 (経過 1001 ms) は timeout 前
    expectEvent(head.update(t0 + 1001), Event::None, "after wrap, before timeout");
    expectEqual(servo.polls, 1, "polled after wrap");
    expectEqual(servo.stops, 0, "no stop before timeout");
    const uint32_t now = t0 + cfg::HEAD_MOVE_TIMEOUT_MS + 1;  // 一周した後の値
    expectTrue(now < t0, "now wrapped");
    expectEvent(head.update(now), Event::Paused, "timeout across wrap");
    // 待ちの終わりも一周をまたいで判定する
    expectEvent(head.update(now + cfg::HEAD_FAULT_RETRY_MS - 1), Event::None, "pause across wrap");
    expectEvent(head.update(now + cfg::HEAD_FAULT_RETRY_MS), Event::Resumed, "resume across wrap");

    // nudge の間隔も一周をまたぐ
    FakeServo servo2;
    HeadLogic head2(servo2);
    start(head2, 0xFFFFFFFFu - 100);
    expectTrue(!head2.nudge(cfg::HEAD_STEP_MAX, 0, 10), "nudge interval across wrap (too soon)");
    expectTrue(head2.nudge(cfg::HEAD_STEP_MAX, 0, cfg::HEAD_STEP_INTERVAL_MS - 101), "nudge interval across wrap");
}

}  // namespace

int main()
{
    testNeutralAndMoveTo();
    testNudge();
    testSettled();
    testNearTargetSettled();
    testPauseAndResume();
    testResumeWithoutPending();
    testFaultLimit();
    testFaultCountResetsBetweenFaults();
    testEnd();
    testLastMotionMs();
    testMillisWrap();
    std::cout << "head_logic_test: ok\n";
    return 0;
}
