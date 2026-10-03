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
#pragma once

#include <cstdint>

namespace photobooth::hw {

class Head {
public:
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
        return !faulted_ && moving_;
    }

    // サーボ応答なしと判定済みか。以後は首を動かさない。
    bool faulted() const
    {
        return faulted_;
    }

    // 最後に首が動いていた時刻: 最後の指示か、isMoving() が true だった最後の問い合わせ。
    // これより後 (+ HEAD_SETTLE_MS) に取ったフレームだけを候補にする。
    uint32_t lastMotionMs() const
    {
        return last_motion_ms_;
    }

    int targetX() const
    {
        return target_x_;
    }
    int targetY() const
    {
        return target_y_;
    }

private:
    void command(int x, int y, uint32_t now_ms);

    int target_x_         = 0;
    int target_y_         = 0;
    uint32_t last_cmd_ms_ = 0;
    bool has_cmd_         = false;
    bool moving_          = false;
    bool watching_        = false;  // 指示後、停止を確認するまで true
    bool faulted_         = false;
    bool paused_          = false;  // 応答なしの直後で、指示を出さずに待っている
    bool pending_         = false;  // 待っている間に来た指示 (target_x_/target_y_) を再開時に送る
    uint32_t paused_since_ms_ = 0;
    int fault_count_      = 0;      // 続けて応答なしになった回数
    bool active_          = false;  // begin() 〜 end() の間
    uint32_t last_poll_ms_ = 0;
    uint32_t last_motion_ms_ = 0;
};

}  // namespace photobooth::hw
