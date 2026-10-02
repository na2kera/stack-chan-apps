/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 撮影フローの状態機械 (docs/design/step1-device.md §4, fw-app-step2.md §4。device/src/app/app の移植)。
//
// 入力イベントと時刻 (millis) だけで遷移する。描画は view::View、ハードウェアは hw::、edge との通信は
// net::EdgeClient を呼ぶだけで、LVGL・HAL・StackChan・Wi-Fi・HTTP を直接触らない。
// onRunning から 1 tick = 1 回 update() され、ブロックしない。
//
// 独立ファーム版との違い (docs/design/fw-app-step1.md §5):
//   - IDLE はランチャーの顔の代わりに「待機」画面。「終了」でアプリを閉じる (exitRequested())。
//   - X_QR の「終了」も IDLE ではなくアプリを閉じてランチャーへ戻る。
//   - ERROR の「再試行」は純正のカメラドライバを作り直さず、取り込みタスクだけ作り直す。
//   - REVIEW の候補 JPEG は非同期で受け取る (EdgeClient::requestCandidate / pollCandidate)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "../hw/audio.h"
#include "../hw/camera.h"
#include "../hw/head.h"
#include "../hw/input.h"
#include "../net/edge_client.h"
#include "../view/view.h"
#include "session.h"
#include "state.h"

namespace photobooth {

class Flow {
public:
    Flow(hw::Camera& camera, hw::Head& head, hw::Audio& audio, view::View& view, net::EdgeClient& edge)
        : camera_(camera), head_(head), audio_(audio), view_(view), edge_(edge)
    {
    }

    // view_ok=false (表示用バッファを確保できない) なら ERROR「メモリ不足」、
    // camera_ok=false なら ERROR (カメラ初期化失敗) から始める。
    void begin(uint32_t now_ms, bool camera_ok, bool view_ok);
    void update(const hw::Event& ev, uint32_t now_ms);
    // アプリを閉じる前の後始末 (取り込み停止・再生停止・候補の解放・未公開セッションの cancel)。
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
    // ERROR の理由の種類。「再試行」の動作が変わる。
    enum class ErrorKind : uint8_t {
        Camera,  // カメラ・表示用バッファ → 取り込みタスク / バッファを作り直す
        Edge,    // 撮影中に edge との通信が切れた → 繋がっていれば新しいセッション、無ければ DIAG
        Upload,  // 写真を保存できない → save を送り直す (回数の上限は EdgeClient が持つ)
        NoPc,    // 判定なしで撮影した写真は保存できない → Edge と同じ
    };
    // REVIEW で edge の応答を待っている段階。待っている間はボタンを出さない。
    enum class ReviewWait : uint8_t {
        None,
        Timeout,    // session_timeout の応答 (候補の有無)
        Candidate,  // 候補 JPEG
    };

    void enter(State next, uint32_t now_ms);
    // judged=true なら edge の顔判定つき (sessionStart を送る)。false は固定フロー。
    void startSession(uint32_t now_ms, bool judged);
    // 撮り直し。判定つきで edge が切れていれば ERROR。
    void retake(uint32_t now_ms);
    // 撮影開始の前に表示用バッファとカメラを確認する。使えなければ ERROR へ進めて false。
    bool checkCamera(uint32_t now_ms);
    // edge との通信が切れたので撮影を止めて ERROR へ。
    void failEdge(uint32_t now_ms, const char* what);
    void failUpload(uint32_t now_ms, const char* reason);
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
    void updateDiag(const hw::Event& ev, uint32_t now_ms);

    // プレビュー 1 フレーム分。CAPTURE なら条件を満たすフレームを候補として保持する。
    // 判定つきなら、首が止まっている (HEAD_SETTLE_MS を過ぎた) フレームだけ edge に渡す。
    // フレームが kFrameStallMs 続けて来なければ ERROR に遷移して false を返す。
    bool previewFrame(bool capture, uint32_t now_ms);
    // edge の frame_result を 1 件処理する (首・案内帯・人数・COMPOSE の安定判定)。
    // CAPTURE で accepted なら true。
    bool handleResult(bool capture, uint32_t now_ms);
    void showHint(net::Hint hint, bool capture);
    const char* bandText(net::Hint hint) const;
    // REVIEW (判定なしの撮影だけ): device の保持フレーム (無ければ「候補の写真がありません」) を出す。
    void showDeviceReview();
    void showReviewNoFace();
    void buildDiag(char* out, size_t len);
    view::IdleLink idleLink() const;
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
    net::EdgeClient& edge_;

    State state_             = State::Idle;
    uint32_t state_since_ms_ = 0;
    Session session_;
    bool judged_             = false;  // 今のセッションは edge の判定つきか
    bool camera_ok_          = false;
    bool view_ok_            = false;  // View::begin() が成功したか (プレビュー・候補の表示に必要)
    bool exit_requested_     = false;

    // IDLE
    bool idle_head_fault_drawn_      = false;  // 待機画面に首の警告を描いたか
    view::IdleLink idle_link_drawn_ = view::IdleLink::Offline;  // 待機画面に描いた接続状態
    uint32_t idle_link_checked_ms_  = 0;

    // COMPOSE の首振りシーケンスの進み (判定なしのときだけ)
    uint8_t sweep_step_ = 0;
    // COMPOSE: 「1 人以上が枠内」が続いている間 true
    bool compose_ok_              = false;
    uint32_t compose_ok_since_ms_ = 0;

    // COMPOSE / CAPTURE の案内帯と人数表示
    net::Hint shown_hint_ = net::Hint::None;
    bool closer_played_   = false;  // closer.wav はセッションで 1 回だけ
    int shown_faces_      = -1;
    int shown_target_     = 0;
    char too_many_text_[32] = {};

    // 首: この frame_id 以前のフレームへの servo_dx/dy は、首を動かす前の画像なので使わない
    uint32_t last_offered_frame_id_ = 0;
    uint32_t nudge_after_frame_id_  = 0;

    // CAPTURE
    int shown_remaining_sec_ = -1;

    // REVIEW
    ReviewWait review_wait_        = ReviewWait::None;
    uint32_t review_wait_since_ms_ = 0;

    // UPLOADING
    bool uploading_captured_ = false;  // 自動採用 (「撮れたよ」を出す)
    bool uploading_quiet_    = false;  // 再試行なので captured.wav を鳴らさない

    // プレビューの実測
    uint32_t preview_frames_   = 0;
    uint32_t offered_frames_   = 0;
    uint32_t preview_since_ms_ = 0;
    // フレーム取得の監視 (COMPOSE / CAPTURE に入るたびにリセット)
    uint32_t last_frame_ms_  = 0;
    uint32_t last_frame_seq_ = 0;

    // 判定なしの撮影の候補フレーム (REVIEW で表示、撮り直し・終了で解放)。判定つきでは使わない
    hw::FrameCopy candidate_;
    // REVIEW で候補を表示できたか (false なら画面は「撮り直す」だけ)
    bool review_has_candidate_ = false;

    // PHOTO_QR / X_QR に出す URL (edge の photo_ready、edge 無効ビルドでは config の固定 URL)
    char photo_url_[256] = {};
    char share_url_[256] = {};
    char expires_at_[8]  = {};  // 表示用 "HH:MM"
    bool photo_ready_    = false;

    // ERROR
    ErrorKind error_kind_     = ErrorKind::Camera;
    const char* error_reason_ = "";
    char error_buf_[224]      = {};

    // DIAG
    char diag_body_[384]        = {};
    uint32_t diag_refreshed_ms_ = 0;
};

}  // namespace photobooth
