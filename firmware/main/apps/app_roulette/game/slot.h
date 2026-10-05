/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// スロットのリールとゲームの状態 (docs/design/app-roulette.md §4 game)。
//
// 移植元 (Moddable 版 miniapp.ts) の Reel と GeekSlotBehavior の状態部分をそのまま移した。
// 値はロゴを 60px から 72px にした分 (1.2 倍) を換算している。
// LVGL / HAL / FreeRTOS に依存しない (firmware/tests/roulette_slot_test.cpp でホストでテストする)。
// 乱数も持たない。開始シンボルはアプリが渡す。
#pragma once

#include <cstdint>

namespace roulette::game {

constexpr float kSymbolSize   = 72.0f;                       // 1 シンボルの高さ (px)
constexpr int kSymbolCount    = 4;                           // シートのロゴの数
constexpr float kStripHeight  = kSymbolSize * kSymbolCount;  // シート全体 (288 px)
constexpr int kReelCount      = 3;
constexpr uint32_t kFrameMs   = 33;     // 1 ステップの長さ (移植元の port.interval)
constexpr float kSpinSpeed    = 15.6f;  // 回転中の速さ (px/ステップ。移植元 13 の 1.2 倍)
constexpr float kStopSlip     = 36.0f;  // 止める指示の後に最低限滑る距離 (移植元 30)
constexpr float kStopMinSpeed = 3.6f;   // 止まりかけの最低速度 (移植元 3)
constexpr float kStopEasing   = 0.32f;  // 残り距離に掛ける減速の係数
constexpr int kMaxStepsPerAdvance = 5;  // advance() 1 回で進める上限 (止まっていた後に一気に進めない)

// スクロール位置をシート内 [0, kStripHeight) へ丸める。
float normalize(float position);

// ペイライン (中央の行) に出ているシンボル番号 (0〜3)。
// 位置 p のときシート座標 p が窓の最上端に来るので、中央の行の先頭は p + kSymbolSize。
int paylineSymbol(float position);

class Reel {
public:
    enum class Phase : uint8_t { Spinning, Stopping, Stopped };

    float position = 0.0f;  // シート座標での窓の上端 (Stopping 中は kStripHeight を超えることがある)
    Phase phase    = Phase::Stopped;
    float target   = 0.0f;  // Stopping で止まる位置 (正規化前)

    // symbol の位置から回し始める。
    void start(int symbol);
    // 止める指示。最低 kStopSlip 滑ってから次のシンボルの区切りで止まる。Spinning のときだけ効く。
    bool requestStop();
    // 1 ステップ進める。
    void update();
};

class Slot {
public:
    enum class Phase : uint8_t { Ready, Spinning, Result };

    // 初期表示で 3 リールが同じ絵柄にならないよう、リール i の位置を i * kSymbolSize にずらしておく。
    Slot();

    Phase phase() const
    {
        return phase_;
    }
    // Result で 3 つ揃ったか。
    bool win() const
    {
        return win_;
    }
    // Spinning 中で、止まったリールがちょうど 2 つ、その中央のシンボルが一致しているか。
    bool reach() const
    {
        return reach_;
    }
    // 前回の呼び出しの後にリーチが始まったか (読むと消える)。advance() が 1 回で複数ステップ進めて、
    // リーチになった直後に 3 つ目が止まった場合でも、演出を出す側が立ち上がりを取りこぼさない。
    bool takeReachStarted();
    // 揃ったシンボル (win() のときだけ。それ以外は -1)。
    int winSymbol() const;
    const Reel& reel(int index) const
    {
        return reels_[index];
    }

    // 3 リールの開始シンボル (0〜3) を受けて回し始める。範囲外は kSymbolCount で丸める。
    // 次の advance() は時刻を覚えるだけになる (開始前の経過を回り始めに乗せない)。
    void start(int r0, int r1, int r2);
    // リール index に止める指示を出す。Spinning のときだけ効く。指示を受け付けたら true。
    bool stopReel(int index);
    // 1 ステップ進める。全リール停止で Result に移って win を決める。
    void step();
    // 前回の呼び出しからの経過を kFrameMs 単位の step() に変換する (最大 kMaxStepsPerAdvance)。
    // Spinning 以外では時刻だけ更新する。進めたステップ数を返す。
    int advance(uint32_t now_ms);

private:
    Reel reels_[kReelCount];
    Phase phase_      = Phase::Ready;
    bool win_         = false;
    bool reach_       = false;
    bool reach_started_ = false;  // リーチの立ち上がり (takeReachStarted() で読むまで残る)
    uint32_t last_ms_ = 0;      // 最後に step() に換算した時刻
    bool has_time_    = false;  // last_ms_ が有効か (最初と start() 直後の advance() では進めない)
};

}  // namespace roulette::game
