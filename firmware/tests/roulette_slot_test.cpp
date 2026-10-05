/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_roulette の game/slot (docs/design/app-roulette.md §6) をホストで確かめる。
#include <apps/app_roulette/game/slot.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using roulette::game::kFrameMs;
using roulette::game::kMaxStepsPerAdvance;
using roulette::game::kReelCount;
using roulette::game::kStopSlip;
using roulette::game::kStripHeight;
using roulette::game::kSymbolCount;
using roulette::game::kSymbolSize;
using roulette::game::paylineSymbol;
using roulette::game::Reel;
using roulette::game::Slot;

void fail(const char* label)
{
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
}

void expectEqual(int actual, int expected, const char* label)
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

// 回転中のリールを止めきるまで step() する (上限つき)。
void runUntilStopped(Reel& reel)
{
    for (int i = 0; i < 1000 && reel.phase != Reel::Phase::Stopped; ++i) {
        reel.update();
    }
    expectTrue(reel.phase == Reel::Phase::Stopped, "reel stops within 1000 steps");
}

// slot の index のリールが止まるまで step() する。
void stepUntilReelStopped(Slot& slot, int index)
{
    for (int i = 0; i < 1000 && slot.reel(index).phase != Reel::Phase::Stopped; ++i) {
        slot.step();
    }
    expectTrue(slot.reel(index).phase == Reel::Phase::Stopped, "slot reel stops within 1000 steps");
}

bool isMultipleOfSymbol(float position)
{
    const float q = position / kSymbolSize;
    return std::fabs(q - std::round(q)) < 1e-4f;
}

void testPaylineSymbol()
{
    // 位置 p で窓の上端がシート座標 p、中央の行はその次のシンボル。
    expectEqual(paylineSymbol(0.0f), 1, "payline 0");
    expectEqual(paylineSymbol(72.0f), 2, "payline 72");
    expectEqual(paylineSymbol(144.0f), 3, "payline 144");
    expectEqual(paylineSymbol(216.0f), 0, "payline 216");
    expectEqual(paylineSymbol(287.9f), 1, "payline 287.9 (rounds to the next symbol = wraps)");
    expectEqual(paylineSymbol(288.0f), 1, "payline 288 (= 0)");
    expectEqual(paylineSymbol(35.0f), 1, "payline 35 (rounds down)");
    expectEqual(paylineSymbol(37.0f), 2, "payline 37 (rounds up)");
    // 負の位置はシート内へ丸めてから見る。
    expectEqual(paylineSymbol(-72.0f), 0, "payline -72 (= 216)");
    expectEqual(paylineSymbol(-1.0f), 1, "payline -1 (= 287)");
    expectEqual(paylineSymbol(-288.0f), 1, "payline -288 (= 0)");
}

void testRequestStop()
{
    // いろいろな位置で止める指示を出し、最低 kStopSlip 滑ってシンボルの区切りで止まることを見る。
    for (int start = 0; start < kSymbolCount; ++start) {
        for (int spins = 0; spins < 40; ++spins) {
            Reel reel;
            reel.start(start);
            for (int i = 0; i < spins; ++i) {
                reel.update();
            }
            const float before = reel.position;
            expectTrue(reel.requestStop(), "requestStop accepted while spinning");
            expectTrue(reel.phase == Reel::Phase::Stopping, "phase is Stopping");
            expectTrue(!reel.requestStop(), "second requestStop ignored");
            // 進んだ距離を足し合わせる (Stopping 中は正規化しないので差がそのまま距離)。
            runUntilStopped(reel);
            const float travelled = reel.target - before;
            expectTrue(travelled >= kStopSlip - 1e-3f, "slides at least kStopSlip");
            expectTrue(travelled < kStopSlip + kSymbolSize + 1e-3f, "stops at the first boundary after slip");
            expectTrue(isMultipleOfSymbol(reel.position), "stopped position is a multiple of kSymbolSize");
            expectTrue(reel.position >= 0.0f && reel.position < kStripHeight, "stopped position is normalized");
        }
    }
    // 止まっているリールには効かない。
    Reel idle;
    expectTrue(!idle.requestStop(), "requestStop ignored when stopped");
}

// 指定のシンボルが中央に来るように 3 リールを止める。
// start(s) の位置 s*72 は中央が (s+1)%4。回したまま止めると位置が変わるので、すぐに止める。
void stopAllImmediately(Slot& slot, int order0, int order1, int order2)
{
    const int order[kReelCount] = {order0, order1, order2};
    for (int k = 0; k < kReelCount; ++k) {
        expectTrue(slot.stopReel(order[k]), "stopReel accepted");
        stepUntilReelStopped(slot, order[k]);
    }
}

void testWinAndLose()
{
    // 3 つとも同じ開始シンボル・同じタイミングで止めれば同じ位置で止まる = 当たり。
    {
        Slot slot;
        slot.start(2, 2, 2);
        expectTrue(slot.phase() == Slot::Phase::Spinning, "spinning after start");
        for (int i = 0; i < kReelCount; ++i) {
            slot.stopReel(i);
        }
        for (int i = 0; i < 100 && slot.phase() == Slot::Phase::Spinning; ++i) {
            slot.step();
        }
        expectTrue(slot.phase() == Slot::Phase::Result, "result after all reels stop");
        expectTrue(slot.win(), "same symbols win");
        expectTrue(!slot.reach(), "no reach in result");
        expectEqual(slot.winSymbol(), paylineSymbol(slot.reel(0).position), "winSymbol is the payline symbol");
    }
    // 開始シンボルが違えば (同時に止めると) 位置が違う = はずれ。
    {
        Slot slot;
        slot.start(0, 1, 2);
        for (int i = 0; i < kReelCount; ++i) {
            slot.stopReel(i);
        }
        for (int i = 0; i < 100 && slot.phase() == Slot::Phase::Spinning; ++i) {
            slot.step();
        }
        expectTrue(slot.phase() == Slot::Phase::Result, "result (lose)");
        expectTrue(!slot.win(), "different symbols lose");
        expectEqual(slot.winSymbol(), -1, "winSymbol is -1 when lost");
    }
    // 再開できる。
    {
        Slot slot;
        slot.start(1, 1, 1);
        stopAllImmediately(slot, 0, 1, 2);
        expectTrue(slot.phase() == Slot::Phase::Result, "result before restart");
        slot.start(3, 0, 1);
        expectTrue(slot.phase() == Slot::Phase::Spinning, "restart spins");
        expectTrue(!slot.win(), "win cleared on restart");
    }
}

// order の順に止めて、2 つ止まった時点のリーチと全停止後を見る。
void checkReachOrder(int a, int b, int c)
{
    Slot slot;
    // a と b は同じ開始シンボル、c は違う開始シンボル。同じステップ数だけ回してから止めると
    // a と b は同じ位置で止まる。
    int starts[kReelCount] = {0, 0, 0};
    starts[c]              = 2;
    slot.start(starts[0], starts[1], starts[2]);
    expectTrue(!slot.reach(), "no reach right after start");

    expectTrue(slot.stopReel(a), "stop a");
    expectTrue(slot.stopReel(b), "stop b");
    // 2 つが止まるまで進める。c は回り続ける。
    for (int i = 0; i < 100 && !(slot.reel(a).phase == Reel::Phase::Stopped &&
                                 slot.reel(b).phase == Reel::Phase::Stopped);
         ++i) {
        expectTrue(!slot.reach(), "no reach while only stopping");
        slot.step();
    }
    expectEqual(paylineSymbol(slot.reel(a).position), paylineSymbol(slot.reel(b).position), "a and b match");
    expectTrue(slot.reel(c).phase == Reel::Phase::Spinning, "c still spinning");
    expectTrue(slot.reach(), "reach with two matching stopped reels");
    // 回っている間はリーチのまま。
    for (int i = 0; i < 10; ++i) {
        slot.step();
        expectTrue(slot.reach(), "reach stays while the last reel spins");
    }
    // 最後のリールを止めると結果に移り、リーチは消える。
    expectTrue(slot.stopReel(c), "stop c");
    for (int i = 0; i < 100 && slot.phase() == Slot::Phase::Spinning; ++i) {
        slot.step();
    }
    expectTrue(slot.phase() == Slot::Phase::Result, "result after the last reel");
    expectTrue(!slot.reach(), "reach cleared after all reels stop");
}

void testReach()
{
    checkReachOrder(0, 1, 2);
    checkReachOrder(0, 2, 1);
    checkReachOrder(1, 2, 0);

    // 止まった 2 つが違えばリーチにならない。
    Slot slot;
    slot.start(0, 1, 0);
    slot.stopReel(0);
    slot.stopReel(1);
    for (int i = 0; i < 100; ++i) {
        slot.step();
        expectTrue(!slot.reach(), "no reach when the two stopped reels differ");
    }
    expectTrue(slot.phase() == Slot::Phase::Spinning, "still spinning (reel 2 not stopped)");

    // 1 つだけ止まってもリーチにならない。
    Slot one;
    one.start(0, 0, 0);
    one.stopReel(0);
    for (int i = 0; i < 100; ++i) {
        one.step();
        expectTrue(!one.reach(), "no reach with one stopped reel");
    }

    // 同じフレームで 3 つ目も止まったらリーチにせず結果へ。
    Slot same;
    same.start(0, 0, 0);
    for (int i = 0; i < kReelCount; ++i) {
        same.stopReel(i);
    }
    bool reached = false;
    for (int i = 0; i < 100 && same.phase() == Slot::Phase::Spinning; ++i) {
        same.step();
        reached = reached || same.reach();
    }
    expectTrue(!reached, "no reach when all reels stop on the same frame");
    expectTrue(same.win(), "win when all stop together");
}

void testStopOnlyWhileSpinning()
{
    Slot slot;
    expectTrue(slot.phase() == Slot::Phase::Ready, "ready at first");
    expectTrue(!slot.stopReel(0), "stopReel ignored in Ready");
    expectTrue(!slot.stopReel(-1), "stopReel ignored for -1");
    slot.start(0, 1, 2);
    expectTrue(!slot.stopReel(3), "stopReel ignored for out-of-range index");
    stopAllImmediately(slot, 2, 1, 0);
    expectTrue(slot.phase() == Slot::Phase::Result, "result");
    expectTrue(!slot.stopReel(0), "stopReel ignored in Result");
}

void testInitialPositions()
{
    Slot slot;
    for (int i = 0; i < kReelCount; ++i) {
        expectTrue(slot.reel(i).position == static_cast<float>(i) * kSymbolSize, "initial position i * kSymbolSize");
        expectTrue(slot.reel(i).phase == Reel::Phase::Stopped, "initially stopped");
    }
}

void testAdvance()
{
    Slot slot;
    // 最初の呼び出しは時刻を覚えるだけ。
    expectEqual(slot.advance(1000), 0, "first advance only records time");
    // Ready では進めない (時刻だけ更新)。
    expectEqual(slot.advance(5000), 0, "no steps in Ready");

    slot.start(0, 1, 2);
    const float p0 = slot.reel(0).position;
    // 前回 (5000) から 32 ms: まだ 1 ステップに満たない。
    expectEqual(slot.advance(5032), 0, "32 ms is less than one frame");
    // 66 ms: 2 ステップ。
    expectEqual(slot.advance(5066), 2, "66 ms = 2 steps");
    expectTrue(std::fabs(slot.reel(0).position - (p0 + 2 * roulette::game::kSpinSpeed)) < 1e-3f,
               "2 steps of spin");
    // 端数は持ち越す: 5066 から 20 ms + 13 ms で 1 ステップ。
    expectEqual(slot.advance(5086), 0, "20 ms carried");
    expectEqual(slot.advance(5099), 1, "carried remainder makes one step");
    // 長く止まっていた: 最大 kMaxStepsPerAdvance。
    expectEqual(slot.advance(5099 + 10 * kFrameMs), kMaxStepsPerAdvance, "capped to max steps");
    // 上限で切ったときは遅れを捨てる (次の呼び出しで残りを取り戻さない)。
    expectEqual(slot.advance(5099 + 10 * kFrameMs + 1), 0, "backlog dropped after cap");
    // ちょうど 5 ステップ分は上限内。
    expectEqual(slot.advance(5099 + 10 * kFrameMs + 1 + 5 * kFrameMs), 5, "exactly 5 steps");
    // millis の一周をまたいでも進む。
    Slot wrap;
    wrap.advance(UINT32_MAX - 10);
    wrap.start(0, 0, 0);
    expectEqual(wrap.advance(UINT32_MAX - 10 + kFrameMs), 1, "advance across millis wrap-around");

    // 結果に移ったら途中で止める。
    Slot fin;
    fin.advance(0);
    fin.start(0, 0, 0);
    for (int i = 0; i < kReelCount; ++i) {
        fin.stopReel(i);
    }
    uint32_t now = 0;
    int total    = 0;
    for (int i = 0; i < 100 && fin.phase() == Slot::Phase::Spinning; ++i) {
        now += 10 * kFrameMs;
        total += fin.advance(now);
    }
    expectTrue(fin.phase() == Slot::Phase::Result, "advance reaches result");
    expectTrue(total > 0, "advance stepped");
    expectEqual(fin.advance(now + 10 * kFrameMs), 0, "no steps in Result");
}

}  // namespace

int main()
{
    testPaylineSymbol();
    testRequestStop();
    testWinAndLose();
    testReach();
    testStopOnlyWhileSpinning();
    testInitialPositions();
    testAdvance();
    std::cout << "roulette_slot_test: ok\n";
    return 0;
}
