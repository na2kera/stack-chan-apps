/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 首の「指示・監視・応答なし判定・一時停止・再開」の状態機械 (docs/design/testable-logic-step1.md §5 D)。
// hw/head.cpp の Head から切り出した。サーボは Servo で受け、時刻は引数で受ける
// (HAL・FreeRTOS・ログに依存しない。firmware/tests/head_logic_test.cpp でホストでテストする)。
// ログは何が起きたか (Event) を返して、呼び出し側 (Head) が出す。
//
// 角度の単位は 1/10 度。X = yaw、Y = pitch。可動域・neutral・ステップ上限は config::HEAD_* で決める。
#pragma once

#include <cstdint>

namespace photobooth::hw {

// 首サーボへの指示と問い合わせ。実機は Head の MotionServo (純正 Motion)、テストでは偽物。
struct Servo {
    virtual ~Servo() = default;
    virtual void moveWithSpeed(int x, int y, int speed) = 0;
    virtual bool isMoving()                              = 0;
    virtual int currentX()                               = 0;
    virtual int currentY()                               = 0;
    virtual void stop()                                  = 0;
};

// 可動域 (HEAD_X_MIN..MAX, HEAD_Y_MIN..MAX) に収める。
int clampHeadX(int x);
int clampHeadY(int y);

class HeadLogic {
public:
    // update() で起きたこと (Head がログを出す)。
    enum class Event : uint8_t {
        None,
        Settled,            // isMoving() が false を返した
        NearTargetSettled,  // HEAD_MOVE_TIMEOUT_MS を過ぎても動作中だが、目標の近くにいるので止まったとみなした
        Paused,             // 目標から離れたまま止まらない。stop() して HEAD_FAULT_RETRY_MS 待つ
        Faulted,            // 続けて HEAD_FAULT_LIMIT 回応答なし。以後首を動かさない
        Resumed,            // 待ちが終わった (待っている間の指示があれば送った)
    };

    // NearTargetSettled / Paused / Faulted のときの角度 (ログ用)。
    struct Detail {
        int cur_x = 0;  // 判定したときの実際の角度
        int cur_y = 0;
        int cmd_x = 0;  // 判定したときの目標 (Paused / Faulted では目標が実際の角度に置き換わる前の値)
        int cmd_y = 0;
    };

    // nudge() で実際に使った変化量 (ログ用)。
    struct Step {
        int dx = 0;
        int dy = 0;
    };

    explicit HeadLogic(Servo& servo) : servo_(servo)
    {
    }

    // 状態を初期化して監視を始める。指示は出さない (呼び出し側が neutral() する)。
    void begin();
    // 動いていなければ (active でなければ) false で何もしない。
    // そうでなければ待ちを取り消し、異常でなければ neutral を指示して、監視をやめる。
    bool end(uint32_t now_ms);

    // 指示から停止までの間だけ isMoving() を問い合わせて応答を監視する。
    Event update(uint32_t now_ms, Detail* detail = nullptr);

    // クランプしてから指示する。
    void moveTo(int x, int y, uint32_t now_ms);
    // 1 回の変化量を ±HEAD_STEP_MAX に制限して相対移動する。
    // 前回指示から HEAD_STEP_INTERVAL_MS 未満、待っている間、変化量が 0 なら何もせず false。
    bool nudge(int dx, int dy, uint32_t now_ms, Step* applied = nullptr);
    // (HEAD_X_NEUTRAL, HEAD_Y_NEUTRAL) へ。
    void neutral(uint32_t now_ms);

    bool active() const
    {
        return active_;
    }
    // 動作中か (update() が取ったキャッシュ)。異常時は false。
    bool isMoving() const
    {
        return !faulted_ && moving_;
    }
    bool faulted() const
    {
        return faulted_;
    }
    // 最後に首が動いていた時刻: 最後の指示か、isMoving() が true だった最後の問い合わせ。
    uint32_t lastMotionMs() const
    {
        return last_motion_ms_;
    }
    // 最後の指示の時刻 (ログの経過時間用)。
    uint32_t lastCommandMs() const
    {
        return last_cmd_ms_;
    }
    // 続けて応答なしになった回数。
    int faultCount() const
    {
        return fault_count_;
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

    Servo& servo_;
    int target_x_             = 0;
    int target_y_             = 0;
    uint32_t last_cmd_ms_     = 0;
    bool has_cmd_             = false;
    bool moving_              = false;
    bool watching_            = false;  // 指示後、停止を確認するまで true
    bool faulted_             = false;
    bool paused_              = false;  // 応答なしの直後で、指示を出さずに待っている
    bool pending_             = false;  // 待っている間に来た指示 (target_x_/target_y_) を再開時に送る
    uint32_t paused_since_ms_ = 0;
    int fault_count_          = 0;      // 続けて応答なしになった回数
    bool active_              = false;  // begin() 〜 end() の間
    uint32_t last_poll_ms_    = 0;
    uint32_t last_motion_ms_  = 0;
};

}  // namespace photobooth::hw
