/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 首サーボのラッパ (docs/design/step1-device.md §5 head。device/src/hal/head の移植)。
//
// 純正の GetStackChan().motion() (stackchan::motion::Motion) を使う。角度の単位は 1/10 度。
// X = yaw、Y = pitch。可動域・neutral・ステップ上限は config::HEAD_* だけで決める。
//
// 独立ファーム (StackChan-BSP) との違い:
//   - 純正 Motion は update() を呼ばないとアニメーションが進まないので、update() の中で
//     motion().update() も呼ぶ (純正アプリが onRunning で GetStackChan().update() するのと同じ役割)。
//   - 純正は「止まったらトルクを自動で抜く」が既定。撮影中に首が垂れないよう begin() で無効にし、
//     end() で neutral に戻す指示を出してから既定 (有効) に戻す。
//
// 指示・監視・応答なし判定・一時停止・再開の判断は HeadLogic (head_logic.h) にあり、
// Head は純正 Motion の呼び出し (MotionServo) と、HeadLogic が返した Event のログだけを持つ。
#pragma once

#include <cstdint>

#include "head_logic.h"

namespace photobooth::hw {

// Servo の実装。純正 GetStackChan().motion() をそのまま呼ぶ。
class MotionServo : public Servo {
public:
    void moveWithSpeed(int x, int y, int speed) override;
    bool isMoving() override;
    int currentX() override;
    int currentY() override;
    void stop() override;
};

class Head {
public:
    Head() = default;
    // logic_ が servo_ を参照するのでコピーしない。
    Head(const Head&)            = delete;
    Head& operator=(const Head&) = delete;

    // neutral() を指示し、以後 update() で応答を監視する。
    void begin(uint32_t now_ms);
    // neutral へ戻す指示を出し、トルク自動解放を純正の既定 (有効) に戻す。
    // 戻る途中のアニメーションはランチャーの GetStackChan().update() が進める。
    void end(uint32_t now_ms);

    // Motion を進め、指示から停止までの間だけ isMoving() を問い合わせて応答を監視する。
    // 指示後 HEAD_MOVE_TIMEOUT_MS を過ぎても動き続けていればサーボ異常とみなす。
    void update(uint32_t now_ms);

    // クランプしてから moveWithSpeed(x, y, HEAD_SPEED)。
    void moveTo(int x, int y, uint32_t now_ms);

    // 1 回の変化量を ±HEAD_STEP_MAX に制限して相対移動する。
    // 前回指示から HEAD_STEP_INTERVAL_MS 未満なら何もせず false。
    bool nudge(int dx, int dy, uint32_t now_ms);

    // (HEAD_X_NEUTRAL, HEAD_Y_NEUTRAL) へ。純正の goHome() は (0, 0) = 下向きなので使わない。
    void neutral(uint32_t now_ms);

    // 動作中か (update() が取ったキャッシュ)。異常時は false (固定カメラ扱い)。
    bool isMoving() const
    {
        return logic_.isMoving();
    }

    // サーボ応答なしと判定済みか。以後は首を動かさない。
    bool faulted() const
    {
        return logic_.faulted();
    }

    // 最後に首が動いていた時刻: 最後の指示か、isMoving() が true だった最後の問い合わせ。
    // これより後 (+ HEAD_SETTLE_MS) に取ったフレームだけを候補にする。
    uint32_t lastMotionMs() const
    {
        return logic_.lastMotionMs();
    }

    int targetX() const
    {
        return logic_.targetX();
    }
    int targetY() const
    {
        return logic_.targetY();
    }

private:
    MotionServo servo_;
    HeadLogic logic_{servo_};
};

}  // namespace photobooth::hw
