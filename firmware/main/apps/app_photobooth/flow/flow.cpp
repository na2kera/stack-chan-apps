/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "flow.h"

#include <mooncake_log.h>

#include "../config.h"
#include "../view/strings.h"

namespace photobooth {

namespace {

constexpr const char* kTag = "PB-Flow";

// ANNOUNCE の保険。isPlaying() が落ちてこない場合でも固まらないようにする
// (announce.wav は 3 秒弱。調整値ではないので config には出さない)。
constexpr uint32_t kAnnounceGuardMs = 15000;
// UPLOADING の保険 (captured.wav の再生完了待ち)。
constexpr uint32_t kUploadingSoundGuardMs = 5000;

// COMPOSE の首振り: neutral → 左に 1 ステップ → 右に 1 ステップ → neutral。
// 1 回の指示は ±HEAD_STEP_MAX に制限されるので、neutral を挟んで 4 回の nudge で表す。
constexpr int kSweep[]          = {-1, +1, +1, -1};
constexpr uint8_t kSweepLen     = sizeof(kSweep) / sizeof(kSweep[0]);

// COMPOSE / CAPTURE でこの時間フレームが 1 枚も来なければカメラ停止とみなす。
constexpr uint32_t kFrameStallMs = 2000;

// ボタンの並び (view と同じ順番)
constexpr int kBtnIdleExit     = 0;  // 待機: 終了
constexpr int kBtnReviewSave   = 0;  // REVIEW: 保存する / 撮り直す
constexpr int kBtnReviewRetake = 1;
constexpr int kBtnReviewOnlyRetake = 0;  // 候補なし: 撮り直す
constexpr int kBtnPhotoNext    = 0;  // PHOTO_QR: 次へ / 撮り直す
constexpr int kBtnPhotoRetake  = 1;
constexpr int kBtnXBack        = 0;  // X_QR: 戻る / 終了
constexpr int kBtnXExit        = 1;
constexpr int kBtnErrorRetry   = 0;  // ERROR: 再試行 / 終了
constexpr int kBtnErrorExit    = 1;

// ステップ1は edge が無いので常に offline (spec §9: 待機画面に「PC未接続」)。
constexpr bool kEdgeOnline = false;

}  // namespace

void Flow::begin(uint32_t now_ms, bool camera_ok, bool view_ok)
{
    camera_ok_      = camera_ok;
    view_ok_        = view_ok;
    exit_requested_ = false;
    state_since_ms_ = now_ms;
    if (!view_ok) {
        // 表示用バッファが無いとプレビューも候補も出せないので、撮影を始めない。
        error_reason_ = str::kErrNoMemory;
        state_        = State::Error;
        mclog::tagWarn(kTag, "start in {}: preview buffer allocation failed", stateName(state_));
        view_.showError(stateTitle(state_), error_reason_);
        return;
    }
    if (!camera_ok) {
        // 前回閉じたときに止めきれなかった取り込みタスクが残っていれば、初期化失敗ではなくそちらを出す。
        error_reason_ = hw::Camera::busy() ? str::kErrCameraBusy : str::kErrCameraInit;
        state_        = State::Error;
        mclog::tagWarn(kTag, "start in {}: camera init failed", stateName(state_));
        view_.showError(stateTitle(state_), error_reason_);
        return;
    }
    state_ = State::Idle;
    mclog::tagInfo(kTag, "start in {}", stateName(state_));
    showIdle();
}

void Flow::end(uint32_t now_ms)
{
    mclog::tagInfo(kTag, "end in {} ({} ms)", stateName(state_), now_ms - state_since_ms_);
    camera_.setStreaming(false);
    audio_.stop();
    candidate_.clear();
    session_.end();
}

void Flow::requestExit(const char* by)
{
    mclog::tagInfo(kTag, "exit requested by {}", by);
    camera_.setStreaming(false);
    exit_requested_ = true;
}

void Flow::enter(State next, uint32_t now_ms)
{
    const State prev = state_;
    mclog::tagInfo(kTag, "state {} -> {} ({} ms)", stateName(prev), stateName(next), now_ms - state_since_ms_);
    if (prev == State::Compose || prev == State::Capture) {
        logPreviewStats(now_ms);
    }
    state_          = next;
    state_since_ms_ = now_ms;

    // 取り込みは COMPOSE / CAPTURE の間だけ回す。
    camera_.setStreaming(camera_ok_ && (next == State::Compose || next == State::Capture));

    switch (next) {
        case State::Idle:
            candidate_.clear();
            session_.end();
            photo_ready_ = false;
            head_.neutral(now_ms);
            showIdle();
            break;

        case State::Announce:
            view_.showAnnounce(stateTitle(next));
            if (!audio_.play(hw::Audio::Clip::Announce)) {
                mclog::tagWarn(kTag, "announce playback failed; continue without voice");
            }
            break;

        case State::Compose:
            sweep_step_       = 0;
            preview_frames_   = 0;
            preview_since_ms_ = now_ms;
            resetFrameWatch(now_ms);
            head_.neutral(now_ms);
            view_.showCompose(camera_ok_);
            break;

        case State::Capture:
            // CAPTURE の 10 秒はここを基準に測る (spec §4)。
            shown_remaining_sec_ = -1;
            preview_frames_      = 0;
            preview_since_ms_    = now_ms;
            resetFrameWatch(now_ms);
            candidate_.clear();
            view_.showCapture(camera_ok_);
            break;

        case State::Review:
            if (candidate_.valid()) {
                mclog::tagInfo(kTag, "review candidate frame_id={} ({}x{}, {} bytes)", candidate_.frameId(),
                               candidate_.width(), candidate_.height(), candidate_.length());
                // 表示できなかったら画面は「撮り直す」だけになるので、ボタンの意味もそれに合わせる。
                review_has_candidate_ =
                    view_.showReview(stateTitle(next), candidate_.pixels(), candidate_.width(), candidate_.height());
            } else {
                mclog::tagWarn(kTag, "review without candidate");
                review_has_candidate_ = view_.showReview(stateTitle(next), nullptr, 0, 0);
            }
            break;

        case State::Uploading:
            photo_ready_ = false;
            view_.showUploading(stateTitle(next));
            audio_.play(hw::Audio::Clip::Captured);
            break;

        case State::PhotoQr:
            view_.showPhotoQr(stateTitle(next), photo_url_, expires_at_);
            break;

        case State::XQr:
            view_.showXQr(stateTitle(next), share_url_);
            break;

        case State::Error:
            view_.showError(stateTitle(next), error_reason_);
            break;
    }
}

void Flow::startSession(uint32_t now_ms)
{
    candidate_.clear();
    session_.start(now_ms);
    mclog::tagInfo(kTag, "session start id={}", session_.id);
}

int Flow::buttonHit(const hw::Event& ev) const
{
    if (ev.kind != hw::Event::Kind::Button || ev.screen != view_.screenId()) {
        return -1;  // 前の画面で押されたボタンは無視する
    }
    return ev.index;
}

void Flow::update(const hw::Event& ev, uint32_t now_ms)
{
    head_.update(now_ms);
    if (exit_requested_) {
        return;
    }
    switch (state_) {
        case State::Idle:
            updateIdle(ev, now_ms);
            break;
        case State::Announce:
            updateAnnounce(now_ms);
            break;
        case State::Compose:
            updateCompose(now_ms);
            break;
        case State::Capture:
            updateCapture(now_ms);
            break;
        case State::Review:
            updateReview(ev, now_ms);
            break;
        case State::Uploading:
            updateUploading(now_ms);
            break;
        case State::PhotoQr:
            updatePhotoQr(ev, now_ms);
            break;
        case State::XQr:
            updateXQr(ev, now_ms);
            break;
        case State::Error:
            updateError(ev, now_ms);
            break;
    }
}

// ---- IDLE (待機) -----------------------------------------------------------

void Flow::updateIdle(const hw::Event& ev, uint32_t now_ms)
{
    if (buttonHit(ev) == kBtnIdleExit) {
        requestExit("idle exit button");
        return;
    }
    // 起動指示 (タッチ) は待機でだけ受ける。画面はボタン以外どこを触っても良い。
    const bool screen_tap = ev.kind == hw::Event::Kind::ScreenTap && ev.screen == view_.screenId();
    if (screen_tap || ev.kind == hw::Event::Kind::HeadTap) {
        mclog::tagInfo(kTag, "start requested by {}", screen_tap ? "screen touch" : "head touch");
        if (!view_ok_) {
            error_reason_ = str::kErrNoMemory;
            enter(State::Error, now_ms);
            return;
        }
        if (!camera_.ready()) {
            error_reason_ = str::kErrCameraInit;
            enter(State::Error, now_ms);
            return;
        }
        startSession(now_ms);
        enter(State::Announce, now_ms);
        return;
    }

    // 首の異常判定は指示から数秒遅れて出るので、変わったら描き直す。
    if (head_.faulted() != idle_head_fault_drawn_) {
        showIdle();
    }
}

void Flow::showIdle()
{
    idle_head_fault_drawn_ = head_.faulted();
    view_.showIdle(kEdgeOnline, idleWarning());
}

const char* Flow::idleWarning() const
{
    if (head_.faulted()) {
        return str::kWarnHeadFault;
    }
    return nullptr;
}

// ---- ANNOUNCE ------------------------------------------------------------

void Flow::updateAnnounce(uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - state_since_ms_;
    if (!audio_.isPlaying()) {
        enter(State::Compose, now_ms);
        return;
    }
    if (elapsed > kAnnounceGuardMs) {
        mclog::tagWarn(kTag, "announce still playing after {} ms; stop and continue", elapsed);
        audio_.stop();
        enter(State::Compose, now_ms);
    }
}

// ---- COMPOSE -------------------------------------------------------------

void Flow::updateCompose(uint32_t now_ms)
{
    if (!previewFrame(false, now_ms)) {
        return;
    }

    // 顔判定が無いので、動作確認用に小さく左右を見るだけ (step1-device.md §4)。
    if (sweep_step_ < kSweepLen && !head_.faulted()) {
        if (head_.nudge(kSweep[sweep_step_] * config::HEAD_STEP_MAX, 0, now_ms)) {
            ++sweep_step_;
        }
    }

    if (now_ms - state_since_ms_ >= config::COMPOSE_TIMEOUT_MS) {
        if (sweep_step_ < kSweepLen && !head_.faulted()) {
            // 首振りが終わらないまま時間切れなら正面に戻してから撮る。
            mclog::tagWarn(kTag, "compose sweep incomplete ({}/{}); back to neutral", sweep_step_, kSweepLen);
            head_.neutral(now_ms);
        }
        enter(State::Capture, now_ms);
    }
}

// ---- CAPTURE -------------------------------------------------------------

void Flow::updateCapture(uint32_t now_ms)
{
    const uint32_t elapsed  = now_ms - state_since_ms_;
    const uint32_t total_ms = config::COUNTDOWN_SEC * 1000;
    if (elapsed >= total_ms) {
        mclog::tagInfo(kTag, "capture timeout: {} ms, candidate frame_id={}", elapsed,
                       candidate_.valid() ? candidate_.frameId() : 0);
        enter(State::Review, now_ms);
        return;
    }

    if (!previewFrame(true, now_ms)) {
        return;
    }

    // 残り秒数 (切り上げ)。変わったときだけ描き直す。
    const int remaining = static_cast<int>((total_ms - elapsed + 999) / 1000);
    if (remaining != shown_remaining_sec_) {
        shown_remaining_sec_ = remaining;
        view_.updateCaptureOverlay(remaining, -1);  // ステップ1は人数不明「--」
    }
}

void Flow::resetFrameWatch(uint32_t now_ms)
{
    last_frame_ms_  = now_ms;
    last_frame_seq_ = camera_.latestSeq();  // 前の状態で取り込んだフレームは使わない
}

bool Flow::previewFrame(bool capture, uint32_t now_ms)
{
    if (!camera_ok_) {
        return true;  // プレビュー枠は状態に入ったときに描いてある
    }
    hw::FrameView frame;
    if (!camera_.lockLatest(last_frame_seq_, frame)) {
        const uint32_t stalled = now_ms - last_frame_ms_;
        if (stalled >= kFrameStallMs) {
            mclog::tagError(kTag, "no camera frame for {} ms in {}", stalled, stateName(state_));
            session_.end();
            error_reason_ = str::kErrCameraNoFrame;
            enter(State::Error, now_ms);
            return false;
        }
        return true;
    }
    last_frame_ms_            = now_ms;
    last_frame_seq_           = frame.seq;
    const uint32_t frame_id   = session_.nextFrameId();
    view_.updatePreview(frame.pixels, frame.width, frame.height);
    // 首が動いている間 (と止まった直後 HEAD_SETTLE_MS) に取ったフレームは候補にしない (spec §4)。
    // 判定は表示した時刻ではなく、カメラから取った時刻 (captured_ms) で行う。
    const bool still = !head_.isMoving() &&
                       static_cast<int32_t>(frame.captured_ms - (head_.lastMotionMs() + config::HEAD_SETTLE_MS)) > 0;
    if (capture && still) {
        candidate_.assign(frame, frame_id);
    }
    camera_.unlock();
    ++preview_frames_;
    return true;
}

void Flow::logPreviewStats(uint32_t now_ms)
{
    const uint32_t dt = now_ms - preview_since_ms_;
    if (dt == 0) {
        return;
    }
    const float fps = preview_frames_ * 1000.0f / dt;
    mclog::tagInfo(kTag, "preview {} frames in {} ms ({:.1f} fps), head target x={} y={}", preview_frames_, dt, fps,
                   head_.targetX(), head_.targetY());
}

// ---- REVIEW --------------------------------------------------------------

void Flow::updateReview(const hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit < 0) {
        return;
    }
    if (review_has_candidate_ && candidate_.valid()) {
        if (hit == kBtnReviewSave) {
            mclog::tagInfo(kTag, "review: save frame_id={}", candidate_.frameId());
            enter(State::Uploading, now_ms);
        } else if (hit == kBtnReviewRetake) {
            mclog::tagInfo(kTag, "review: retake");
            startSession(now_ms);
            enter(State::Announce, now_ms);
        }
    } else if (hit == kBtnReviewOnlyRetake) {
        mclog::tagInfo(kTag, "review: retake (no candidate)");
        startSession(now_ms);
        enter(State::Announce, now_ms);
    }
}

// ---- UPLOADING -----------------------------------------------------------

void Flow::updateUploading(uint32_t now_ms)
{
    if (!photo_ready_) {
        // ステップ1: edge が無いので設定ファイルの固定 URL を使う。削除時刻は未定。
        photo_url_   = config::FIXED_PHOTO_URL;
        share_url_   = config::FIXED_SHARE_URL;
        expires_at_  = str::kExpiresUnknown;
        photo_ready_ = true;
        mclog::tagInfo(kTag, "photo ready (expires {})", expires_at_);  // URL/トークンはログに出さない
    }
    // 「撮れたよ」を言い切ってから QR に進む。
    const bool sound_done = !audio_.isPlaying() || now_ms - state_since_ms_ > kUploadingSoundGuardMs;
    if (photo_ready_ && sound_done) {
        enter(State::PhotoQr, now_ms);
    }
}

// ---- PHOTO_QR / X_QR -----------------------------------------------------

void Flow::updatePhotoQr(const hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit == kBtnPhotoNext) {
        enter(State::XQr, now_ms);
    } else if (hit == kBtnPhotoRetake) {
        mclog::tagInfo(kTag, "photo qr: retake");
        startSession(now_ms);
        enter(State::Announce, now_ms);
    }
}

void Flow::updateXQr(const hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit == kBtnXBack) {
        enter(State::PhotoQr, now_ms);
    } else if (hit == kBtnXExit) {
        mclog::tagInfo(kTag, "session end id={}", session_.id);
        candidate_.clear();
        session_.end();
        requestExit("x_qr exit button");
    }
}

// ---- ERROR ---------------------------------------------------------------

void Flow::updateError(const hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit == kBtnErrorRetry) {
        mclog::tagInfo(kTag, "error: retry ({})", error_reason_);
        if (!view_ok_) {
            // メモリ不足: 表示用バッファの確保からやり直す。
            view_ok_ = view_.begin();
            if (!view_ok_) {
                mclog::tagError(kTag, "retry failed: preview buffer still unavailable");
                error_reason_ = str::kErrNoMemory;
                view_.showError(stateTitle(state_), error_reason_);
                return;
            }
            if (camera_ok_) {
                enter(State::Idle, now_ms);
                return;
            }
            // カメラも使えないままなら、続けてカメラをやり直す。
        }
        // 純正のカメラドライバはボードが持っているので作り直さない。取り込みタスクだけ作り直す。
        camera_ok_ = camera_.restart();
        if (camera_ok_) {
            enter(State::Idle, now_ms);
        } else {
            // 前の取り込みタスクが切り離されたまま生きている間は、二重に回さないよう再起動しない。
            if (hw::Camera::busy()) {
                error_reason_ = str::kErrCameraBusy;
            }
            mclog::tagError(kTag, "retry failed: camera not available ({})",
                            hw::Camera::busy() ? "previous task still running" : "no camera");
            view_.showError(stateTitle(state_), error_reason_);
        }
    } else if (hit == kBtnErrorExit) {
        session_.end();
        enter(State::Idle, now_ms);
    }
}

}  // namespace photobooth
