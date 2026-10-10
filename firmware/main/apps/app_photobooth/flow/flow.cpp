/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "flow.h"

#include <mooncake_log.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "../config.h"
#include "../net/link_logic.h"
#include "../view/strings.h"
#include "time_format.h"

namespace photobooth {

namespace {

constexpr const char* kTag = "PB-Flow";

// edge 無効ビルド (config_local.h なし / -DPHOTOBOOTH_NO_EDGE=1) ならステップ1の固定フロー
// (固定 URL の QR) のまま動かす。
constexpr bool kEdgeEnabled = config::EDGE_ENABLED;

// ANNOUNCE の保険。isPlaying() が落ちてこない場合でも固まらないようにする
// (announce.wav は 3 秒弱。調整値ではないので config には出さない)。
constexpr uint32_t kAnnounceGuardMs = 15000;
// UPLOADING の保険 (captured.wav の再生完了待ち)。
constexpr uint32_t kUploadingSoundGuardMs = 5000;
// REVIEW で session_timeout の応答を待つ上限。送信中のフレーム (最大 EDGE_TIMEOUT_MS) と
// timeout 本体 (通信失敗時は 1 回送り直す) の分。
constexpr uint32_t kReviewWaitMs = config::EDGE_TIMEOUT_MS * 3;
// REVIEW で候補 JPEG を待つ上限の保険 (通常は EdgeClient::pollCandidate() が先に諦める)。
constexpr uint32_t kCandidateGuardMs = config::EDGE_TIMEOUT_MS * 2;
// SHUTTER の保険。isPlaying() が落ちてこない場合でも固まらないようにする
// (shutter.wav は 240 ms。調整値ではないので config には出さない)。
constexpr uint32_t kShutterSoundGuardMs = 1000;
// DIAG の本文を作り直す間隔 (変わったときだけ描く)。
constexpr uint32_t kDiagRefreshMs = 1000;
// 待機画面の接続表示を確かめる間隔。
constexpr uint32_t kIdleLinkPollMs = 200;

// COMPOSE の首振り: neutral → 左に 1 ステップ → 右に 1 ステップ → neutral。
// 1 回の指示は ±HEAD_STEP_MAX に制限されるので、neutral を挟んで 4 回の nudge で表す。
// edge の判定なし (固定フロー) のときだけ使う。判定つきでは edge の servo_dx/dy に従う。
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
constexpr int kBtnErrorOnlyExit = 0;  // ERROR (再試行なし): 終了
constexpr int kBtnDiagReconnect = 0;  // DIAG: 再接続 / 判定なしで撮影 (戻るは頭部タッチ)
constexpr int kBtnDiagShoot     = 1;

// SHUTTER のログ用: 撮れた写真として何を出したか。
const char* capturedName(view::CapturedSource src)
{
    switch (src) {
        case view::CapturedSource::Candidate:
            return "edge candidate";
        case view::CapturedSource::Preview:
            return "preview fallback";
        case view::CapturedSource::None:
            break;
    }
    return "no image";
}

}  // namespace

void Flow::begin(uint32_t now_ms, bool camera_ok, bool view_ok)
{
    camera_ok_      = camera_ok;
    view_ok_        = view_ok;
    exit_requested_ = false;
    state_since_ms_ = now_ms;
    snprintf(too_many_text_, sizeof(too_many_text_), "%u%s", static_cast<unsigned>(config::MAX_FACES),
             str::kBandTooManySuffix);
    if (!view_ok) {
        // 表示用バッファが無いとプレビューも候補も出せないので、撮影を始めない。
        error_kind_   = ErrorKind::Camera;
        error_reason_ = str::kErrNoMemory;
        state_        = State::Error;
        mclog::tagWarn(kTag, "start in {}: preview buffer allocation failed", stateName(state_));
        view_.showError(stateTitle(state_), error_reason_);
        return;
    }
    if (!camera_ok) {
        // 前回閉じたときに止めきれなかった取り込みタスクが残っていれば、初期化失敗ではなくそちらを出す。
        error_kind_   = ErrorKind::Camera;
        error_reason_ = hw::Camera::busy() ? str::kErrCameraBusy : str::kErrCameraInit;
        state_        = State::Error;
        mclog::tagWarn(kTag, "start in {}: camera init failed", stateName(state_));
        view_.showError(stateTitle(state_), error_reason_);
        return;
    }
    state_ = State::Idle;
    mclog::tagInfo(kTag, "start in {} (edge {})", stateName(state_), kEdgeEnabled ? "enabled" : "disabled");
    showIdle();
}

void Flow::end(uint32_t now_ms)
{
    mclog::tagInfo(kTag, "end in {} ({} ms)", stateName(state_), now_ms - state_since_ms_);
    camera_.setStreaming(false);
    audio_.stop();
    candidate_.clear();
    // SHUTTER で用意したまま出していない候補 JPEG。edge 側の受け取り口は edge_.end() が解放する。
    view_.discardCapturedJpeg();
    cand_drain_ = false;
    if (session_.active && judged_ && !photo_ready_) {
        // 公開前に閉じた: 未公開の候補を edge に捨てさせる (届かなくても 5 分で破棄される)。
        // QR を出した後 (公開済み) は送らない。
        edge_.sessionCancel(session_);
    }
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
    if (prev == State::Shutter && next != State::Shutter) {
        view_.discardCapturedJpeg();  // 表示しなかった候補 JPEG を持ち越さない
        if (shutter_cand_ == ShutterCandidate::Waiting) {
            cand_drain_ = true;  // 後から届く JPEG は受け取って捨てる
        }
    }
    state_          = next;
    state_since_ms_ = now_ms;

    // 取り込みは COMPOSE / CAPTURE の間だけ回す。
    camera_.setStreaming(camera_ok_ && (next == State::Compose || next == State::Capture));

    switch (next) {
        case State::Idle:
            candidate_.clear();
            session_.end();
            judged_      = false;
            photo_ready_ = false;
            head_.neutral(now_ms);
            showIdle();
            break;

        case State::Announce:
            view_.showAnnounce(stateTitle(next));
            if (!audio_.play(clips_.get(hw::Clip::Announce))) {
                mclog::tagWarn(kTag, "announce playback failed; continue without voice");
            }
            break;

        case State::Compose:
            sweep_step_            = 0;
            compose_ok_            = false;
            shown_hint_            = net::Hint::None;
            preview_frames_        = 0;
            offered_frames_        = 0;
            preview_since_ms_      = now_ms;
            last_offered_frame_id_ = 0;
            nudge_after_frame_id_  = 0;
            resetFrameWatch(now_ms);
            head_.neutral(now_ms);
            view_.showCompose(camera_ok_);
            break;

        case State::Capture:
            // CAPTURE の 10 秒はここを基準に測る (spec §4)。
            shown_remaining_sec_ = -1;
            shown_faces_         = -1;
            shown_target_        = 0;
            shown_hint_          = net::Hint::None;
            preview_frames_      = 0;
            offered_frames_      = 0;
            preview_since_ms_    = now_ms;
            resetFrameWatch(now_ms);
            candidate_.clear();
            view_.showCapture(camera_ok_);
            break;

        case State::Shutter:
            // CAPTURE の最後のフレームは preview バッファに残っている (取り込みは上で止めた)。
            shutter_phase_         = ShutterPhase::Flash;
            shutter_shown_         = view::CapturedSource::None;
            shutter_hold_since_ms_ = now_ms;
            mclog::tagInfo(kTag, "shutter: enter (flash {} ms, hold >= {} ms, candidate {})", config::SHUTTER_FLASH_MS,
                           config::CAPTURED_HOLD_MS, shutter_cand_ == ShutterCandidate::Waiting ? "requested" : "none");
            view_.showShutterFlash();
            shutter_sound_since_ms_ = now_ms;
            if (audio_.play(clips_.get(hw::Clip::Shutter))) {
                shutter_sound_ = ShutterSound::Shutter;
            } else if (audio_.play(clips_.get(hw::Clip::Captured))) {
                mclog::tagWarn(kTag, "shutter: shutter sound unavailable; play captured voice");
                shutter_sound_ = ShutterSound::Captured;
            } else {
                mclog::tagWarn(kTag, "shutter: audio unavailable; continue without sound");
                shutter_sound_ = ShutterSound::Done;
            }
            break;

        case State::Review:
            review_has_candidate_ = false;
            if (review_wait_ == ReviewWait::Timeout) {
                // edge の timeout 応答を待つ間は最後のプレビューのまま (ボタンは出さない)。
                mclog::tagInfo(kTag, "review: waiting for edge timeout result");
                review_wait_since_ms_ = now_ms;
                view_.setBand(nullptr);
                view_.updateCaptureOverlay(0, shown_faces_, shown_target_);
            } else {
                showDeviceReview();
            }
            break;

        case State::Uploading:
            photo_ready_ = false;
            view_.showUploading(stateTitle(next), uploading_captured_);
            // 判定なしの撮影は保存できないので「撮れたよ」は言わない。再試行でも言い直さない。
            if (!uploading_quiet_ && !(kEdgeEnabled && !judged_)) {
                audio_.play(clips_.get(hw::Clip::Captured));
            }
            uploading_quiet_ = false;
            break;

        case State::PhotoQr:
            if (!view_.showPhotoQr(stateTitle(next), photo_url_, expires_at_)) {
                failQr(now_ms, "photo");
                return;
            }
            break;

        case State::XQr:
            if (!view_.showXQr(stateTitle(next), share_url_)) {
                failQr(now_ms, "share");
                return;
            }
            break;

        case State::Error:
            view_.showError(stateTitle(next), error_reason_, error_kind_ != ErrorKind::Qr);
            break;

        case State::Diag:
            buildDiag(diag_body_, sizeof(diag_body_));
            diag_refreshed_ms_ = now_ms;
            view_.showDiag(stateTitle(next), diag_body_);
            break;
    }
}

void Flow::startSession(uint32_t now_ms, bool judged)
{
    candidate_.clear();
    session_.start(now_ms);
    judged_             = judged;
    photo_ready_        = false;  // QR 画面からの撮り直し: 前のセッションの「公開済み」を持ち越さない
    closer_played_      = false;
    review_wait_        = ReviewWait::None;
    uploading_captured_ = false;
    uploading_quiet_    = false;
    mclog::tagInfo(kTag, "session start id={} ({})", session_.id, judged ? "edge judge" : "no judge");
    if (judged) {
        edge_.sessionStart(session_);
    }
}

void Flow::retake(uint32_t now_ms)
{
    const bool online = kEdgeEnabled && edge_.isOnline();
    if (judged_ && !online) {
        failEdge(now_ms, str::kErrEdgeLost);
        return;
    }
    // 判定なしで撮っていても、edge が戻っていれば判定つきで撮り直す。
    startSession(now_ms, online);
    enter(State::Announce, now_ms);
}

bool Flow::checkCamera(uint32_t now_ms)
{
    if (!view_ok_) {
        error_kind_   = ErrorKind::Camera;
        error_reason_ = str::kErrNoMemory;
        enter(State::Error, now_ms);
        return false;
    }
    if (!camera_.ready()) {
        error_kind_   = ErrorKind::Camera;
        error_reason_ = str::kErrCameraInit;
        enter(State::Error, now_ms);
        return false;
    }
    return true;
}

void Flow::failEdge(uint32_t now_ms, const char* what)
{
    const char* detail = edge_.lastError();
    mclog::tagWarn(kTag, "{} in {}: {}", what, stateName(state_), detail);
    if (session_.active && judged_) {
        edge_.sessionCancel(session_);  // 届かなくてもよい (edge は 5 分で破棄する)
    }
    session_.end();
    review_wait_ = ReviewWait::None;
    snprintf(error_buf_, sizeof(error_buf_), "%s\n%s", what, detail);
    error_reason_ = error_buf_;
    error_kind_   = ErrorKind::Edge;
    enter(State::Error, now_ms);
}

void Flow::failUpload(uint32_t now_ms, const char* reason)
{
    mclog::tagWarn(kTag, "upload failed: {}", reason);
    if (strcmp(reason, net::kSaveRetryExhausted) == 0) {
        // 送信回数の上限は EdgeClient が数える (保存の再試行は 1 か所だけ)。
        snprintf(error_buf_, sizeof(error_buf_), "%s\n%s\n%s", str::kErrUploadFailed, str::kErrRetryExhausted,
                 str::kErrRetakeGuide);
    } else {
        snprintf(error_buf_, sizeof(error_buf_), "%s\n(%s)", str::kErrUploadFailed, reason);
    }
    error_reason_ = error_buf_;
    error_kind_   = ErrorKind::Upload;
    enter(State::Error, now_ms);
}

void Flow::failQr(uint32_t now_ms, const char* which)
{
    // URL が QR に入らないなど。読めない画面を出したままにしない (URL 自体はログに出さない)。
    mclog::tagError(kTag, "{} QR cannot be shown", which);
    error_kind_   = ErrorKind::Qr;
    error_reason_ = str::kErrQr;
    enter(State::Error, now_ms);
}

int Flow::buttonHit(const shared::hw::Event& ev) const
{
    if (ev.kind != shared::hw::Event::Kind::Button || ev.screen != view_.screenId()) {
        return -1;  // 前の画面で押されたボタンは無視する
    }
    return ev.index;
}

void Flow::requestCandidate(uint32_t timeout_ms, bool allow_retry)
{
    cand_drain_ = false;  // 新しい依頼で前の依頼の結果は EdgeClient が捨てる
    edge_.requestCandidate(session_, timeout_ms, allow_retry);
}

void Flow::drainCandidate()
{
    if (!cand_drain_) {
        return;
    }
    bool ok = false;
    net::JpegBytes jpeg;
    if (edge_.pollCandidate(ok, jpeg)) {
        cand_drain_ = false;
        mclog::tagInfo(kTag, "shutter: late candidate {} ({} bytes); discarded", ok ? "arrived" : "failed",
                       jpeg.size());
    }  // jpeg はここで解放される
}

void Flow::update(const shared::hw::Event& ev, uint32_t now_ms)
{
    // 判定つきの撮影の開始から写真の準備完了までは、net タスクが定期 hello を止める (後ろに並ぶ
    // timeout / candidate / save / photo を待たせない)。QR を出した後は hello を再開して warm を保ち、
    // edge の停止にも気づけるようにする (「撮り直す」が古い online 判定で始まらないように)。
    edge_.setLatencySensitive(session_.active && judged_ && !photo_ready_);
    head_.update(now_ms);
    if (exit_requested_) {
        return;
    }
    drainCandidate();
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
        case State::Shutter:
            updateShutter(now_ms);
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
        case State::Diag:
            updateDiag(ev, now_ms);
            break;
    }
}

// ---- IDLE (待機) -----------------------------------------------------------

void Flow::updateIdle(const shared::hw::Event& ev, uint32_t now_ms)
{
    if (buttonHit(ev) == kBtnIdleExit) {
        requestExit("idle exit button");
        return;
    }
    // 起動指示 (タッチ) は待機でだけ受ける。画面はボタン以外どこを触っても良い。
    const bool screen_tap = ev.kind == shared::hw::Event::Kind::ScreenTap && ev.screen == view_.screenId();
    if (screen_tap || ev.kind == shared::hw::Event::Kind::HeadTap) {
        mclog::tagInfo(kTag, "start requested by {}", screen_tap ? "screen touch" : "head touch");
        if (!kEdgeEnabled) {
            // ステップ1と同じ固定フロー
            if (!checkCamera(now_ms)) return;
            startSession(now_ms, false);
            enter(State::Announce, now_ms);
            return;
        }
        switch (net::link::idleTouch(edge_.linkState())) {
            case net::link::IdleTouch::Start:
                break;
            case net::link::IdleTouch::Ignore:
                // 最初の hello の結果待ち・cold start (「準備中」)。診断は開かず待機のまま (予約もしない)。
                mclog::tagInfo(kTag, "edge starting; touch ignored");
                return;
            case net::link::IdleTouch::OpenDiag:
                // spec §9: edge 不通なら自動判定つきの撮影は始めず、診断画面を開く。
                mclog::tagInfo(kTag, "edge offline ({}); open diagnostics", edge_.lastError());
                enter(State::Diag, now_ms);
                return;
        }
        if (!checkCamera(now_ms)) return;
        startSession(now_ms, true);
        enter(State::Announce, now_ms);
        return;
    }

    // 首の異常判定は指示から数秒遅れて出るので、変わったら描き直す。
    if (head_.faulted() != idle_head_fault_drawn_) {
        showIdle();
        return;
    }

    // 右下の接続表示 (「接続中」/「準備中」/「接続なし」)。変わったときだけ差し替える。
    if (now_ms - idle_link_checked_ms_ >= kIdleLinkPollMs) {
        idle_link_checked_ms_     = now_ms;
        const view::IdleLink link = idleLink();
        if (link != idle_link_drawn_) {
            idle_link_drawn_ = link;
            mclog::tagInfo(kTag, "idle: edge {}",
                           link == view::IdleLink::Online     ? "online"
                           : link == view::IdleLink::Starting ? "starting"
                                                              : "offline");
            view_.updateIdleStatus(link);
        }
    }
}

view::IdleLink Flow::idleLink() const
{
    switch (edge_.linkState()) {
        case net::LinkState::Online:
            return view::IdleLink::Online;
        case net::LinkState::Starting:
            return view::IdleLink::Starting;
        case net::LinkState::Offline:
            break;
    }
    return view::IdleLink::Offline;
}

void Flow::showIdle()
{
    idle_head_fault_drawn_ = head_.faulted();
    idle_link_drawn_       = idleLink();
    view_.showIdle(idle_link_drawn_, idleWarning());
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

    if (judged_) {
        if (!edge_.isOnline()) {
            failEdge(now_ms, str::kErrEdgeLost);
            return;
        }
        handleResult(false, now_ms);
        // spec §4: 1 人以上が枠内に 1 秒連続で入ったら CAPTURE。
        if (compose_ok_ && now_ms - compose_ok_since_ms_ >= config::COMPOSE_STABLE_MS) {
            mclog::tagInfo(kTag, "compose stable for {} ms", now_ms - compose_ok_since_ms_);
            enter(State::Capture, now_ms);
            return;
        }
    } else if (sweep_step_ < kSweepLen && !head_.faulted()) {
        // 顔判定が無いので、動作確認用に小さく左右を見るだけ (step1-device.md §4)。
        if (head_.nudge(kSweep[sweep_step_] * config::HEAD_STEP_MAX, 0, now_ms)) {
            ++sweep_step_;
        }
    }

    if (now_ms - state_since_ms_ >= config::COMPOSE_TIMEOUT_MS) {
        if (!judged_ && sweep_step_ < kSweepLen && !head_.faulted()) {
            // 首振りが終わらないまま時間切れなら正面に戻してから撮る。
            mclog::tagWarn(kTag, "compose sweep incomplete ({}/{}); back to neutral", sweep_step_, kSweepLen);
            head_.neutral(now_ms);
        }
        mclog::tagInfo(kTag, "compose timeout");
        enter(State::Capture, now_ms);
    }
}

bool Flow::handleResult(bool capture, uint32_t now_ms)
{
    net::FrameResult r;
    if (!edge_.pollResult(r) || !r.valid || r.dropped) {
        return false;
    }

    // 首: edge の値は「希望」。nudge() が可動域・1 回の上限・間隔でクランプする。
    // 前回首を動かす前に撮ったフレームへの指示は、もう古いので使わない。
    if ((r.servo_dx != 0 || r.servo_dy != 0) && !head_.faulted() && r.frame_id > nudge_after_frame_id_) {
        if (head_.nudge(r.servo_dx, r.servo_dy, now_ms)) {
            nudge_after_frame_id_ = last_offered_frame_id_;
        }
    }

    showHint(r.hint, capture);

    if (!capture) {
        const bool ok = r.face_count >= 1 && r.all_in_frame;
        if (ok && !compose_ok_) {
            compose_ok_          = true;
            compose_ok_since_ms_ = now_ms;
        } else if (!ok) {
            compose_ok_ = false;
        }
        return false;
    }

    if (r.face_count != shown_faces_ || r.target_face_count != shown_target_) {
        shown_faces_  = r.face_count;
        shown_target_ = r.target_face_count;
        view_.updateCaptureOverlay(shown_remaining_sec_, shown_faces_, shown_target_);
    }
    return r.accepted;
}

const char* Flow::bandText(net::Hint hint) const
{
    switch (hint) {
        case net::Hint::Closer:
            return str::kBandCloser;
        case net::Hint::TooMany:
            return too_many_text_;
        case net::Hint::None:
            break;
    }
    return str::kCompose;
}

void Flow::showHint(net::Hint hint, bool capture)
{
    if (hint == shown_hint_) {
        return;
    }
    shown_hint_ = hint;
    // COMPOSE は帯を出したまま文言を替える。CAPTURE は hint があるときだけ帯を出す。
    if (capture && hint == net::Hint::None) {
        view_.setBand(nullptr);
    } else {
        view_.setBand(bandText(hint));
    }
    if (hint == net::Hint::Closer && !closer_played_) {
        closer_played_ = true;
        audio_.play(clips_.get(hw::Clip::Closer));
    }
}

// ---- CAPTURE -------------------------------------------------------------

void Flow::updateCapture(uint32_t now_ms)
{
    const uint32_t elapsed  = now_ms - state_since_ms_;
    const uint32_t total_ms = config::COUNTDOWN_SEC * 1000;
    if (elapsed >= total_ms) {
        mclog::tagInfo(kTag, "capture timeout: {} ms ({})", elapsed, judged_ ? "ask edge for candidate" : "device candidate");
        review_wait_ = ReviewWait::None;
        if (judged_) {
            // 10 秒経過後に届いた結果は採用しない (spec §6.2)。ここで捨てる。
            net::FrameResult late;
            if (edge_.pollResult(late) && late.valid && late.accepted) {
                mclog::tagWarn(kTag, "ignore accepted frame_id={} after {} ms", late.frame_id, elapsed);
            }
            edge_.sessionTimeout(session_);
            review_wait_ = ReviewWait::Timeout;
        }
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
        view_.updateCaptureOverlay(remaining, shown_faces_, shown_target_);
    }

    if (!judged_) {
        return;
    }
    if (!edge_.isOnline()) {
        failEdge(now_ms, str::kErrEdgeLost);
        return;
    }
    if (handleResult(true, now_ms)) {
        mclog::tagInfo(kTag, "frame accepted by edge at {} ms (faces {}/{})", elapsed, shown_faces_, shown_target_);
        // SHUTTER に出す採用フレームの JPEG を先に依頼する。edge はコマンドを順に処理し、公開が終わると
        // フレームを捨てる (candidate が 404 になる) ので、save より前に取りに行く。
        // 1 回だけ・短い期限で取りに行き、後ろの save を待たせない (失敗ならプレビューを止めて見せる)。
        requestCandidate(config::SHUTTER_CANDIDATE_TIMEOUT_MS, false);
        shutter_cand_ = ShutterCandidate::Waiting;
        // accepted でも save を送る (edge は自動ではアップロードしない、protocol.md)。
        edge_.reviewDecision(session_, true);
        uploading_captured_ = true;
        enter(State::Shutter, now_ms);
    }
}

// ---- SHUTTER (自動採用の直後の演出) -----------------------------------------
//
// 白フラッシュ (SHUTTER_FLASH_MS) → 撮れた写真 + タイトル帯「撮れたよ」(ボタンなし)。
// 音は shutter.wav → captured.wav。写真を CAPTURED_HOLD_MS 以上出し、captured.wav が終わったら
// UPLOADING へ (save は CAPTURE で送ってあるので、UPLOADING は photo_ready を待つだけ)。
// 写真は edge の候補 JPEG (採用フレーム)。フラッシュの終わりまでに届かなければ最後のプレビューを止めて
// 出し、後から届いたら差し替える。フレームの結果・CAPTURE の 10 秒の期限はここでは扱わない。

void Flow::updateShutter(uint32_t now_ms)
{
    // 採用後に届いた frame_result は使わない (首も動かさない)。
    net::FrameResult late;
    (void)edge_.pollResult(late);

    pollShutterCandidate(now_ms);

    const uint32_t elapsed = now_ms - state_since_ms_;
    if (shutter_phase_ == ShutterPhase::Flash) {
        if (elapsed >= config::SHUTTER_FLASH_MS) {
            showShutterCaptured(now_ms);
        }
    } else if (shutter_cand_ == ShutterCandidate::Ready) {
        // 最後のプレビューを出している間に候補が届いた: 実際に保存される写真に差し替える。
        showShutterCaptured(now_ms);
    }

    // 音: shutter.wav が終わったら captured.wav。
    if (shutter_sound_ == ShutterSound::Shutter &&
        (!audio_.isPlaying() || now_ms - shutter_sound_since_ms_ > kShutterSoundGuardMs)) {
        if (audio_.isPlaying()) {
            mclog::tagWarn(kTag, "shutter: shutter sound still playing after {} ms", now_ms - shutter_sound_since_ms_);
        }
        mclog::tagInfo(kTag, "shutter: shutter sound {} ms; play captured voice", now_ms - shutter_sound_since_ms_);
        shutter_sound_since_ms_ = now_ms;
        shutter_sound_ = audio_.play(clips_.get(hw::Clip::Captured)) ? ShutterSound::Captured : ShutterSound::Done;
    } else if (shutter_sound_ == ShutterSound::Captured &&
               (!audio_.isPlaying() || now_ms - shutter_sound_since_ms_ > kUploadingSoundGuardMs)) {
        if (audio_.isPlaying()) {
            mclog::tagWarn(kTag, "shutter: captured voice still playing after {} ms", now_ms - shutter_sound_since_ms_);
        }
        mclog::tagInfo(kTag, "shutter: captured voice {} ms", now_ms - shutter_sound_since_ms_);
        shutter_sound_ = ShutterSound::Done;
    }

    if (shutter_phase_ == ShutterPhase::Hold && shutter_sound_ == ShutterSound::Done &&
        now_ms - shutter_hold_since_ms_ >= config::CAPTURED_HOLD_MS) {
        mclog::tagInfo(kTag, "shutter: done in {} ms (photo shown {} ms, {})", elapsed, now_ms - shutter_hold_since_ms_,
                       capturedName(shutter_shown_));
        // captured.wav は鳴らし終えたので UPLOADING では鳴らさない。「撮れたよ」の表示は今まで通り。
        uploading_captured_ = true;
        uploading_quiet_    = true;
        enter(State::Uploading, now_ms);
    }
}

void Flow::pollShutterCandidate(uint32_t now_ms)
{
    if (shutter_cand_ != ShutterCandidate::Waiting) {
        return;
    }
    const uint32_t waited = now_ms - state_since_ms_;
    bool ok               = false;
    net::JpegBytes jpeg;
    const bool done = edge_.pollCandidate(ok, jpeg);
    if (!done) {
        if (waited >= kCandidateGuardMs) {
            // 通常は EdgeClient::pollCandidate() が先に諦める。届いたら drainCandidate() で捨てる。
            mclog::tagWarn(kTag, "shutter: candidate not received in {} ms; keep preview", waited);
            shutter_cand_ = ShutterCandidate::Failed;
            cand_drain_   = true;
        }
        return;
    }
    // デコード結果は View が持つ (表示するまで画面は変えない)。jpeg はここで解放される。
    if (ok && view_.prepareCapturedJpeg(jpeg.data(), jpeg.size())) {
        mclog::tagInfo(kTag, "shutter: edge candidate jpeg ({} bytes) ready after {} ms", jpeg.size(), waited);
        shutter_cand_ = ShutterCandidate::Ready;
    } else {
        mclog::tagWarn(kTag, "shutter: edge candidate cannot be shown after {} ms ({}); keep preview", waited,
                       ok ? "decode" : edge_.lastError());
        shutter_cand_ = ShutterCandidate::Failed;
    }
}

void Flow::showShutterCaptured(uint32_t now_ms)
{
    const view::CapturedSource prev = shutter_shown_;
    shutter_shown_                  = view_.showCaptured(stateTitle(State::Shutter));
    if (shutter_shown_ == view::CapturedSource::Candidate) {
        shutter_cand_ = ShutterCandidate::Shown;
    }
    const char* what = capturedName(shutter_shown_);
    if (shutter_phase_ == ShutterPhase::Flash) {
        shutter_phase_         = ShutterPhase::Hold;
        shutter_hold_since_ms_ = now_ms;
        mclog::tagInfo(kTag, "shutter: flash {} ms; show {}", now_ms - state_since_ms_, what);
    } else {
        // 写真を差し替えても止めて見せる時間は延ばさない (差し替え前と同じ場面なので)。
        mclog::tagInfo(kTag, "shutter: {} -> {} at {} ms", capturedName(prev), what, now_ms - state_since_ms_);
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
            if (session_.active && judged_) {
                edge_.sessionCancel(session_);
            }
            session_.end();
            error_kind_   = ErrorKind::Camera;
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
    // device 側で候補を持つのは判定なしの撮影だけ。判定つきでは edge が保持するフレームだけが
    // 保存対象なので、device の別フレームを「保存する」付きで見せない (画面と保存される写真をずらさない)。
    if (capture && still && !judged_) {
        candidate_.assign(frame, frame_id);
    }
    // edge の顔判定。首が動いている間・止まった直後のフレームは送らない (ブレたフレームを判定・採用しない)。
    // offerFrame() はスロットへコピーして即 return する (直後に unlock() でフレームを返すため)。
    if (judged_ && still) {
        if (edge_.offerFrame(session_, frame, head_.targetX(), head_.targetY(),
                             capture ? net::Phase::Capture : net::Phase::Compose)) {
            last_offered_frame_id_ = frame_id;
            ++offered_frames_;
        }
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
    mclog::tagInfo(kTag, "preview {} frames in {} ms ({:.1f} fps), offered to edge {}, head target x={} y={}",
                   preview_frames_, dt, fps, offered_frames_, head_.targetX(), head_.targetY());
}

// ---- REVIEW --------------------------------------------------------------

void Flow::showDeviceReview()
{
    if (candidate_.valid()) {
        mclog::tagInfo(kTag, "review device candidate frame_id={} ({}x{}, {} bytes)", candidate_.frameId(),
                       candidate_.width(), candidate_.height(), candidate_.length());
        // 表示できなかったら画面は「撮り直す」だけになるので、ボタンの意味もそれに合わせる。
        review_has_candidate_ = view_.showReview(stateTitle(State::Review), candidate_.pixels(), candidate_.width(),
                                                 candidate_.height());
    } else {
        // 判定なしの撮影で、首が止まっているフレームが 1 枚も取れなかった (顔の有無は分からない)。
        mclog::tagWarn(kTag, "review without candidate");
        review_has_candidate_ = view_.showReview(stateTitle(State::Review), nullptr, 0, 0);
    }
}

void Flow::showReviewNoFace()
{
    // edge は顔 0 のフレームを候補にしないので、候補なし = 10 秒間顔が見つからなかった (spec §9)。
    // 保存できないので「撮り直す」だけ。
    mclog::tagInfo(kTag, "review: edge has no candidate (no face)");
    review_has_candidate_ = false;
    view_.showReviewEmpty(stateTitle(State::Review), str::kNoFace);
}

void Flow::updateReview(const shared::hw::Event& ev, uint32_t now_ms)
{
    if (review_wait_ == ReviewWait::Timeout) {
        bool ok            = false;
        bool has_candidate = false;
        if (edge_.pollTimeout(ok, has_candidate)) {
            if (!ok) {
                // 通信失敗・edge 再起動後の 404 など。「候補なし」と区別して撮影を止める。
                failEdge(now_ms, str::kErrEdgeLost);
                return;
            }
            if (!has_candidate) {
                review_wait_ = ReviewWait::None;
                showReviewNoFace();
                return;
            }
            // edge が保持している同じフレームを JPEG で受け取って表示する (protocol.md)。
            requestCandidate();
            review_wait_          = ReviewWait::Candidate;
            review_wait_since_ms_ = now_ms;
        } else if (now_ms - review_wait_since_ms_ >= kReviewWaitMs) {
            failEdge(now_ms, str::kErrEdgeLost);
        }
        return;  // 待っている間のタッチは無視 (ボタンは出ていない)
    }
    if (review_wait_ == ReviewWait::Candidate) {
        bool ok = false;
        net::JpegBytes jpeg;
        const bool done = edge_.pollCandidate(ok, jpeg);
        if (!done && now_ms - review_wait_since_ms_ < kCandidateGuardMs) {
            return;
        }
        review_wait_ = ReviewWait::None;
        if (done && ok && view_.showReviewJpeg(stateTitle(State::Review), jpeg.data(), jpeg.size())) {
            mclog::tagInfo(kTag, "review: edge candidate jpeg ({} bytes)", jpeg.size());
            review_has_candidate_ = true;
            return;
        }
        // edge には候補があるが、受け取れない・デコードできない・大きさが違う。見せられない写真を
        // 保存させない: 「候補の写真を表示できません」と「撮り直す」だけにする。
        mclog::tagWarn(kTag, "review: edge candidate cannot be shown ({}); retake only", edge_.lastError());
        review_has_candidate_ = false;
        view_.showReviewEmpty(stateTitle(State::Review), str::kCandidateUnavailable);
        return;
    }

    const int hit = buttonHit(ev);
    if (hit < 0) {
        return;
    }
    if (review_has_candidate_) {
        if (hit == kBtnReviewSave) {
            mclog::tagInfo(kTag, "review: save");
            if (judged_) {
                edge_.reviewDecision(session_, true);
            }
            uploading_captured_ = false;
            enter(State::Uploading, now_ms);
        } else if (hit == kBtnReviewRetake) {
            mclog::tagInfo(kTag, "review: retake");
            if (judged_) {
                edge_.reviewDecision(session_, false);
            }
            retake(now_ms);
        }
    } else if (hit == kBtnReviewOnlyRetake) {
        mclog::tagInfo(kTag, "review: retake (no candidate)");
        if (judged_) {
            edge_.reviewDecision(session_, false);
        }
        retake(now_ms);
    }
}

// ---- UPLOADING -----------------------------------------------------------

void Flow::updateUploading(uint32_t now_ms)
{
    if (!photo_ready_) {
        if (!kEdgeEnabled) {
            // edge 無効ビルド: ステップ1と同じ設定ファイルの固定 URL。削除時刻は未定。
            snprintf(photo_url_, sizeof(photo_url_), "%s", config::FIXED_PHOTO_URL);
            snprintf(share_url_, sizeof(share_url_), "%s", config::FIXED_SHARE_URL);
            snprintf(expires_at_, sizeof(expires_at_), "%s", str::kExpiresUnknown);
            photo_ready_ = true;
        } else if (!judged_) {
            // 「判定なしで撮影」は edge に写真が無いので保存できない (QR を捏造しない、spec §9)。
            error_kind_   = ErrorKind::NoPc;
            error_reason_ = str::kErrNoPc;
            enter(State::Error, now_ms);
            return;
        } else {
            net::PhotoInfo info;
            if (edge_.pollPhotoReady(info)) {
                if (info.status != net::PhotoInfo::Status::Ready) {
                    failUpload(now_ms, info.reason[0] ? info.reason : "error");
                    return;
                }
                snprintf(photo_url_, sizeof(photo_url_), "%s", info.photo_url);
                snprintf(share_url_, sizeof(share_url_), "%s", info.share_url);
                flow::formatExpires(info.expires_at, expires_at_, sizeof(expires_at_));
                photo_ready_ = true;
            } else if (now_ms - state_since_ms_ >= config::UPLOAD_WAIT_MS) {
                failUpload(now_ms, "timeout");
                return;
            }
        }
        if (photo_ready_) {
            // URL/トークンはログに出さない
            mclog::tagInfo(kTag, "photo ready (expires {}) in {} ms", static_cast<const char*>(expires_at_),
                           now_ms - state_since_ms_);
        }
    }
    // 「撮れたよ」を言い切ってから QR に進む。
    const bool sound_done = !audio_.isPlaying() || now_ms - state_since_ms_ > kUploadingSoundGuardMs;
    if (photo_ready_ && sound_done) {
        enter(State::PhotoQr, now_ms);
    }
}

// ---- PHOTO_QR / X_QR -----------------------------------------------------

void Flow::updatePhotoQr(const shared::hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit == kBtnPhotoNext) {
        enter(State::XQr, now_ms);
    } else if (hit == kBtnPhotoRetake) {
        mclog::tagInfo(kTag, "photo qr: retake");
        retake(now_ms);
    }
}

void Flow::updateXQr(const shared::hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (hit == kBtnXBack) {
        enter(State::PhotoQr, now_ms);
    } else if (hit == kBtnXExit) {
        mclog::tagInfo(kTag, "session end id={}", session_.id);
        candidate_.clear();
        session_.end();  // 公開済みなので cancel は送らない
        requestExit("x_qr exit button");
    }
}

// ---- ERROR ---------------------------------------------------------------

void Flow::updateError(const shared::hw::Event& ev, uint32_t now_ms)
{
    const int hit = buttonHit(ev);
    if (error_kind_ == ErrorKind::Qr) {
        // 「終了」だけの画面 (ボタン 0)。写真は公開済みなので cancel は送らない。
        if (hit == kBtnErrorOnlyExit) {
            enter(State::Idle, now_ms);
        }
        return;
    }
    if (hit == kBtnErrorRetry) {
        mclog::tagInfo(kTag, "error: retry ({})", error_reason_);
        switch (error_kind_) {
            case ErrorKind::Camera:
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
                break;

            case ErrorKind::Upload:
                // 同じ session_id で save を送り直す (edge は冪等)。回数を使い切っていれば EdgeClient が
                // 送らずに retry_exhausted を返すので、UPLOADING からすぐ ERROR に戻る。
                edge_.reviewDecision(session_, true);
                uploading_captured_ = false;
                uploading_quiet_    = true;
                enter(State::Uploading, now_ms);
                break;

            case ErrorKind::Qr:
                break;  // 上で処理済み (再試行ボタンは無い)

            case ErrorKind::Edge:
            case ErrorKind::NoPc:
                if (kEdgeEnabled && edge_.isOnline()) {
                    if (!checkCamera(now_ms)) return;
                    startSession(now_ms, true);
                    enter(State::Announce, now_ms);
                } else {
                    enter(State::Diag, now_ms);
                }
                break;
        }
    } else if (hit == kBtnErrorExit) {
        if (session_.active && judged_ && !photo_ready_) {
            edge_.sessionCancel(session_);
        }
        enter(State::Idle, now_ms);
    }
}

// ---- DIAG ----------------------------------------------------------------

void Flow::buildDiag(char* out, size_t len)
{
    net::Diagnostics d;
    edge_.diagnostics(d);
    const char* wifi = str::kDiagWifiDisconnected;
    switch (d.wifi) {
        case net::Diagnostics::Wifi::Connected:
            wifi = str::kDiagWifiConnected;
            break;
        case net::Diagnostics::Wifi::ConfigMode:
            wifi = str::kDiagWifiConfigMode;
            break;
        case net::Diagnostics::Wifi::NotConfigured:
            wifi = str::kDiagWifiNotConfigured;
            break;
        case net::Diagnostics::Wifi::Disconnected:
            break;
    }
    const bool connected = d.wifi == net::Diagnostics::Wifi::Connected;
    char line_wifi[80];
    char line_ip[48];
    if (connected) {
        snprintf(line_wifi, sizeof(line_wifi), "%s: %s (%s)", str::kDiagWifiLabel, wifi, d.ssid);
        snprintf(line_ip, sizeof(line_ip), "IP: %s  %d dBm", d.ip[0] ? d.ip : "-", d.rssi);
    } else {
        snprintf(line_wifi, sizeof(line_wifi), "%s: %s", str::kDiagWifiLabel, wifi);
        snprintf(line_ip, sizeof(line_ip), "IP: -");
    }
    // 接続先 (scheme://host:port) は鍵ではないので画面には出す (PC の IP が DHCP で変わったことに気づけるように)。
    snprintf(out, len, "%s\n%s\n%s: %s\n%s: %s\n%s: %s", line_wifi, line_ip, str::kDiagPcLabel,
             d.edge_url[0] ? d.edge_url : "-", str::kDiagReplyLabel, d.online ? str::kDiagReplyYes : str::kDiagReplyNo,
             str::kDiagErrorLabel, d.last_error[0] ? d.last_error : str::kDiagNone);
}

void Flow::updateDiag(const shared::hw::Event& ev, uint32_t now_ms)
{
    if (ev.kind == shared::hw::Event::Kind::HeadTap) {  // 戻る (ボタン帯は 2 つまで)
        enter(State::Idle, now_ms);
        return;
    }
    const int hit = buttonHit(ev);
    if (hit == kBtnDiagReconnect) {
        mclog::tagInfo(kTag, "diag: reconnect");
        edge_.reconnect();
    } else if (hit == kBtnDiagShoot) {
        mclog::tagInfo(kTag, "diag: shoot without edge judge");
        if (!checkCamera(now_ms)) return;
        startSession(now_ms, false);
        enter(State::Announce, now_ms);
        return;
    }

    if (now_ms - diag_refreshed_ms_ >= kDiagRefreshMs || hit == kBtnDiagReconnect) {
        diag_refreshed_ms_ = now_ms;
        char body[sizeof(diag_body_)];
        buildDiag(body, sizeof(body));
        if (strcmp(body, diag_body_) != 0) {
            memcpy(diag_body_, body, sizeof(diag_body_));
            view_.updateDiagBody(diag_body_);
        }
    }
}

}  // namespace photobooth
