/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 撮影フローの状態機械 (docs/design/step1-device.md §4。device/src/app/app の移植)。
//
// 入力イベントと時刻 (millis) だけで遷移する。描画は view::View、ハードウェアは hw:: を呼ぶだけで、
// LVGL・HAL・StackChan を直接触らない。onRunning から 1 tick = 1 回 update() され、ブロックしない。
//
// 独立ファーム版との違い (docs/design/fw-app-step1.md §5):
//   - IDLE はランチャーの顔の代わりに「待機」画面。「終了」でアプリを閉じる (exitRequested())。
//   - X_QR の「終了」も IDLE ではなくアプリを閉じてランチャーへ戻る。
//   - ERROR の「再試行」は純正のカメラドライバを作り直さず、取り込みタスクだけ作り直す。
#pragma once

#include <cstdint>

#include "../hw/audio.h"
#include "../hw/camera.h"
#include "../hw/head.h"
#include "../hw/input.h"
#include "../view/view.h"
#include "session.h"
#include "state.h"

namespace photobooth {

class Flow {
public:
    Flow(hw::Camera& camera, hw::Head& head, hw::Audio& audio, view::View& view)
        : camera_(camera), head_(head), audio_(audio), view_(view)
    {
    }

    // view_ok=false (表示用バッファを確保できない) なら ERROR「メモリ不足」、
    // camera_ok=false なら ERROR (カメラ初期化失敗) から始める。
    void begin(uint32_t now_ms, bool camera_ok, bool view_ok);
    void update(const hw::Event& ev, uint32_t now_ms);
    // アプリを閉じる前の後始末 (取り込み停止・再生停止・候補の解放)。
    void end(uint32_t now_ms);

    State state() const
    {
        return state_;
    }
    // 「終了」が押されてアプリを閉じるべきか。
    bool exitRequested() const
    {
        return exit_requested_;
    }

private:
    void enter(State next, uint32_t now_ms);
    void startSession(uint32_t now_ms);
    void requestExit(const char* by);

    void updateIdle(const hw::Event& ev, uint32_t now_ms);
    void updateAnnounce(uint32_t now_ms);
    void updateCompose(uint32_t now_ms);
    void updateCapture(uint32_t now_ms);
    void updateReview(const hw::Event& ev, uint32_t now_ms);
    void updateUploading(uint32_t now_ms);
    void updatePhotoQr(const hw::Event& ev, uint32_t now_ms);
    void updateXQr(const hw::Event& ev, uint32_t now_ms);
    void updateError(const hw::Event& ev, uint32_t now_ms);

    // プレビュー 1 フレーム分。CAPTURE なら条件を満たすフレームを候補として保持する。
    // フレームが kFrameStallMs 続けて来なければ ERROR に遷移して false を返す。
    bool previewFrame(bool capture, uint32_t now_ms);
    void resetFrameWatch(uint32_t now_ms);
    void logPreviewStats(uint32_t now_ms);
    void showIdle();
    const char* idleWarning() const;
    // 今の画面のボタンが押されたなら index、そうでなければ -1。
    int buttonHit(const hw::Event& ev) const;

    hw::Camera& camera_;
    hw::Head& head_;
    hw::Audio& audio_;
    view::View& view_;

    State state_             = State::Idle;
    uint32_t state_since_ms_ = 0;
    Session session_;
    bool camera_ok_          = false;
    bool view_ok_            = false;  // View::begin() が成功したか (プレビュー・候補の表示に必要)
    bool exit_requested_     = false;

    // IDLE
    bool idle_head_fault_drawn_ = false;  // 待機画面に首の警告を描いたか

    // COMPOSE の首振りシーケンスの進み
    uint8_t sweep_step_ = 0;

    // CAPTURE
    int shown_remaining_sec_ = -1;

    // プレビューの実測
    uint32_t preview_frames_   = 0;
    uint32_t preview_since_ms_ = 0;
    // フレーム取得の監視 (COMPOSE / CAPTURE に入るたびにリセット)
    uint32_t last_frame_ms_  = 0;
    uint32_t last_frame_seq_ = 0;

    // 候補フレーム (REVIEW で表示、撮り直し・終了で解放)
    hw::FrameCopy candidate_;
    // REVIEW で候補を表示できたか (false なら画面は「撮り直す」だけ)
    bool review_has_candidate_ = false;

    // PHOTO_QR / X_QR に出す URL
    const char* photo_url_  = "";
    const char* share_url_  = "";
    const char* expires_at_ = "";
    bool photo_ready_       = false;

    const char* error_reason_ = "";
};

}  // namespace photobooth
