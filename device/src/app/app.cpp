#include "app/app.h"

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "ui/screens.h"

namespace app {

namespace {

constexpr const char* TAG = "app";

// -DPHOTOBOOTH_NO_EDGE (NullEdge) ならステップ1の固定フロー (固定 URL の QR) のまま動かす。
#ifdef PHOTOBOOTH_NO_EDGE
constexpr bool kEdgeEnabled = false;
#else
constexpr bool kEdgeEnabled = true;
#endif

// ANNOUNCE の保険。isPlaying() が落ちてこない場合でも固まらないようにする
// (announce.wav は 3 秒弱。調整値ではないので config には出さない)。
constexpr uint32_t kAnnounceGuardMs = 15000;
// UPLOADING の保険 (captured.wav の再生完了待ち)。
constexpr uint32_t kUploadingSoundGuardMs = 5000;
// REVIEW で session_timeout の応答を待つ上限。送信中のフレーム (最大 EDGE_TIMEOUT_MS) と
// timeout 本体 (通信失敗時は 1 回送り直す) の分。
constexpr uint32_t kReviewWaitMs = config::EDGE_TIMEOUT_MS * 3;
// DIAG の本文を描き直す間隔 (変わったときだけ描く)。
constexpr uint32_t kDiagRefreshMs = 1000;

// COMPOSE の首振り: neutral → 左に 1 ステップ → 右に 1 ステップ → neutral。
// 1 回の指示は ±HEAD_STEP_MAX に制限されるので、neutral を挟んで 4 回の nudge で表す。
// edge の判定なし (固定フロー) のときだけ使う。判定つきでは edge の servo_dx/dy に従う。
constexpr int kSweep[] = {-1, +1, +1, -1};
constexpr uint8_t kSweepLen = sizeof(kSweep) / sizeof(kSweep[0]);

constexpr const char* kCameraInitFailed = "カメラ初期化失敗";
constexpr const char* kCameraNoFrame = "カメラからフレームを取得できません";
constexpr const char* kEdgeLost = "PCとの接続が切れました";
constexpr const char* kNoPc = "PC未接続のため保存できません";
constexpr const char* kUploadFailed = "写真を保存できませんでした";
constexpr const char* kBandCloser = "もう少し寄ってね";

// COMPOSE / CAPTURE でこの時間フレームが 1 枚も取れなければカメラ停止とみなす。
constexpr uint32_t kFrameStallMs = 2000;

// "2026-09-30T22:00:00+09:00" → "22:00"。形が違えば "--:--"。
String formatExpires(const char* iso) {
  if (iso == nullptr || strlen(iso) < 16 || iso[10] != 'T' || iso[13] != ':') {
    return String("--:--");
  }
  char hhmm[6];
  memcpy(hhmm, iso + 11, 5);
  hhmm[5] = '\0';
  return String(hhmm);
}

}  // namespace

void App::begin(uint32_t now_ms, bool camera_ok) {
  ui::begin();
  snprintf(too_many_text_, sizeof(too_many_text_), "%u人までだよ",
           static_cast<unsigned>(config::MAX_FACES));
  state_since_ms_ = now_ms;
  if (!camera_ok) {
    error_kind_ = ErrorKind::Camera;
    error_reason_ = kCameraInitFailed;
    state_ = State::Error;
    ESP_LOGW(TAG, "start in %s: %s", stateName(state_), error_reason_);
    ui::drawError(stateTitle(state_), error_reason_);
    return;
  }
  state_ = State::Idle;
  ESP_LOGI(TAG, "start in %s (edge %s)", stateName(state_), kEdgeEnabled ? "http" : "disabled");
  drawIdle(now_ms);
}

void App::enter(State next, uint32_t now_ms) {
  const State prev = state_;
  ESP_LOGI(TAG, "state %s -> %s (%lu ms)", stateName(prev), stateName(next),
           static_cast<unsigned long>(now_ms - state_since_ms_));
  if (prev == State::Compose || prev == State::Capture) {
    logPreviewStats(now_ms);
  }
  state_ = next;
  state_since_ms_ = now_ms;

  switch (next) {
    case State::Idle:
      candidate_.clear();
      session_.end();
      judged_ = false;
      photo_ready_ = false;
      head_.neutral(now_ms);
      drawIdle(now_ms);
      break;

    case State::Announce:
      // セリフ再生中はマイクを止める (speakerOn が Mic.end() を含む)。
      audio_.speakerOn();
      ui::drawAnnounce(stateTitle(next));
      if (!audio_.playAnnounce()) {
        ESP_LOGW(TAG, "announce playback failed; continue without voice");
      }
      break;

    case State::Compose:
      sweep_step_ = 0;
      compose_ok_ = false;
      shown_hint_ = edge::Hint::None;
      preview_frames_ = 0;
      offered_frames_ = 0;
      preview_since_ms_ = now_ms;
      last_offered_frame_id_ = 0;
      nudge_after_frame_id_ = 0;
      resetFrameWatch(now_ms);
      head_.neutral(now_ms);
      if (!camera_.ready()) {
        ui::drawPreviewPlaceholder();
      }
      ui::drawComposeOverlay(ui::kBandDefault);
      break;

    case State::Capture:
      // CAPTURE の 10 秒はここを基準に測る (spec §4)。
      shown_remaining_sec_ = -1;
      shown_faces_ = -1;
      shown_target_ = 0;
      shown_hint_ = edge::Hint::None;
      capture_band_ = false;
      preview_frames_ = 0;
      offered_frames_ = 0;
      preview_since_ms_ = now_ms;
      resetFrameWatch(now_ms);
      candidate_.clear();
      if (!camera_.ready()) {
        ui::drawPreviewPlaceholder();
      }
      break;

    case State::Review:
      review_buttons_ = 0;
      if (review_waiting_) {
        // edge の timeout 応答を待つ間は最後のプレビューのまま (ボタンは出さない)。
        ESP_LOGI(TAG, "review: waiting for edge timeout result");
        ui::drawCaptureOverlay(0, shown_faces_, shown_target_);
      } else {
        drawDeviceReview();
      }
      break;

    case State::Uploading:
      photo_ready_ = false;
      ui::drawUploading(stateTitle(next), uploading_captured_);
      // 判定なしの撮影は保存できないので「撮れたよ」は言わない。再試行でも言い直さない。
      if (!uploading_quiet_ && !(kEdgeEnabled && !judged_)) {
        audio_.playCaptured();
      }
      uploading_quiet_ = false;
      break;

    case State::PhotoQr:
      ui::drawPhotoQr(stateTitle(next), photo_url_.c_str(), expires_at_.c_str());
      break;

    case State::XQr:
      ui::drawXQr(stateTitle(next), share_url_.c_str());
      break;

    case State::Error:
      ui::drawError(stateTitle(next), error_reason_);
      break;

    case State::Diag:
      buildDiag(diag_body_, sizeof(diag_body_));
      diag_refreshed_ms_ = now_ms;
      ui::drawDiag(stateTitle(next), diag_body_);
      break;
  }
}

void App::startSession(uint32_t now_ms, bool judged) {
  candidate_.clear();
  session_.start(now_ms);
  judged_ = judged;
  upload_retries_ = 0;
  closer_played_ = false;
  ESP_LOGI(TAG, "session start id=%s (%s)", session_.id, judged ? "edge judge" : "no judge");
  if (judged) {
    edge_.sessionStart(session_);
  }
}

void App::retake(uint32_t now_ms) {
  const bool online = kEdgeEnabled && edge_.isOnline();
  if (judged_ && !online) {
    failEdge(now_ms, kEdgeLost);
    return;
  }
  // 判定なしで撮っていても、edge が戻っていれば判定つきで撮り直す。
  startSession(now_ms, online);
  enter(State::Announce, now_ms);
}

bool App::checkCamera(uint32_t now_ms) {
  if (!camera_.ready() && !hal::Camera::disabled()) {
    error_kind_ = ErrorKind::Camera;
    error_reason_ = kCameraInitFailed;
    enter(State::Error, now_ms);
    return false;
  }
  return true;
}

void App::failEdge(uint32_t now_ms, const char* what) {
  const char* detail = edge_.lastError();
  ESP_LOGW(TAG, "%s in %s: %s", what, stateName(state_), detail);
  if (session_.active && judged_) {
    edge_.sessionCancel(session_);  // 届かなくてもよい (edge は 5 分で破棄する)
  }
  session_.end();
  snprintf(error_buf_, sizeof(error_buf_), "%s\n%s", what, detail);
  error_reason_ = error_buf_;
  error_kind_ = ErrorKind::Edge;
  enter(State::Error, now_ms);
}

void App::failUpload(uint32_t now_ms, const char* reason) {
  ESP_LOGW(TAG, "upload failed: %s (retries %u/%u)", reason, upload_retries_,
           static_cast<unsigned>(config::UPLOAD_RETRY));
  if (upload_retries_ < config::UPLOAD_RETRY) {
    snprintf(error_buf_, sizeof(error_buf_), "%s\n(%s)", kUploadFailed, reason);
  } else {
    snprintf(error_buf_, sizeof(error_buf_), "%s\n(%s) 終了して撮り直してね", kUploadFailed,
             reason);
  }
  error_reason_ = error_buf_;
  error_kind_ = ErrorKind::Upload;
  enter(State::Error, now_ms);
}

void App::update(const hal::Event& ev, uint32_t now_ms) {
  head_.update(now_ms);
  switch (state_) {
    case State::Idle:      updateIdle(ev, now_ms); break;
    case State::Announce:  updateAnnounce(now_ms); break;
    case State::Compose:   updateCompose(now_ms); break;
    case State::Capture:   updateCapture(now_ms); break;
    case State::Review:    updateReview(ev, now_ms); break;
    case State::Uploading: updateUploading(now_ms); break;
    case State::PhotoQr:   updatePhotoQr(ev, now_ms); break;
    case State::XQr:       updateXQr(ev, now_ms); break;
    case State::Error:     updateError(ev, now_ms); break;
    case State::Diag:      updateDiag(ev, now_ms); break;
  }
}

// ---- IDLE ----------------------------------------------------------------

void App::updateIdle(const hal::Event& ev, uint32_t now_ms) {
  // 起動指示 (タッチ) は IDLE でだけ受ける。画面はどこを触っても良い。
  if (ev.kind == hal::Event::Kind::ScreenTap || ev.kind == hal::Event::Kind::HeadTap) {
    ESP_LOGI(TAG, "start requested by %s",
             ev.kind == hal::Event::Kind::HeadTap ? "head touch" : "screen touch");
    if (!kEdgeEnabled) {
      // ステップ1と同じ固定フロー
      if (!checkCamera(now_ms)) return;
      startSession(now_ms, false);
      enter(State::Announce, now_ms);
      return;
    }
    if (!edge_.isOnline()) {
      // spec §9: edge 不通なら自動判定つきの撮影は始めず、診断画面を開く。
      ESP_LOGI(TAG, "edge offline (%s); open diagnostics", edge_.lastError());
      enter(State::Diag, now_ms);
      return;
    }
    if (!checkCamera(now_ms)) return;
    startSession(now_ms, true);
    enter(State::Announce, now_ms);
    return;
  }

  // 起動直後の首の異常判定は数秒遅れて出るので、変わったら描き直す。
  if (head_.faulted() != idle_head_fault_drawn_) {
    drawIdle(now_ms);
    return;
  }

  const bool online = edge_.isOnline();
  if (online != idle_online_drawn_) {
    idle_online_drawn_ = online;
    ESP_LOGI(TAG, "idle: edge %s", online ? "online" : "offline");
    ui::updateIdleStatus(online);
  }

  // 瞬き: 開いている時間 IDLE_BLINK_INTERVAL_MS、閉じている時間 IDLE_BLINK_MS。
  const uint32_t hold = eyes_open_ ? config::IDLE_BLINK_INTERVAL_MS : config::IDLE_BLINK_MS;
  if (now_ms - eyes_changed_ms_ >= hold) {
    eyes_open_ = !eyes_open_;
    eyes_changed_ms_ = now_ms;
    ui::updateIdleEyes(eyes_open_);
  }
}

void App::drawIdle(uint32_t now_ms) {
  eyes_open_ = true;
  eyes_changed_ms_ = now_ms;
  idle_head_fault_drawn_ = head_.faulted();
  idle_online_drawn_ = edge_.isOnline();
  ui::drawIdle(idle_online_drawn_, idleWarning());
}

const char* App::idleWarning() const {
  if (head_.faulted()) {
    return "首モーター応答なし";
  }
  if (hal::Camera::disabled()) {
    return "カメラ無効ビルド";
  }
  return nullptr;
}

// ---- ANNOUNCE ------------------------------------------------------------

void App::updateAnnounce(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - state_since_ms_;
  if (!audio_.isPlaying()) {
    enter(State::Compose, now_ms);
    return;
  }
  if (elapsed > kAnnounceGuardMs) {
    ESP_LOGW(TAG, "announce still playing after %lu ms; stop and continue",
             static_cast<unsigned long>(elapsed));
    audio_.stop();
    enter(State::Compose, now_ms);
  }
}

// ---- COMPOSE -------------------------------------------------------------

void App::updateCompose(uint32_t now_ms) {
  if (!previewFrame(false, now_ms)) {
    return;
  }

  if (judged_) {
    if (!edge_.isOnline()) {
      failEdge(now_ms, kEdgeLost);
      return;
    }
    handleResult(false, now_ms);
    // spec §4: 1 人以上が枠内に 1 秒連続で入ったら CAPTURE。
    if (compose_ok_ && now_ms - compose_ok_since_ms_ >= config::COMPOSE_STABLE_MS) {
      ESP_LOGI(TAG, "compose stable for %lu ms", static_cast<unsigned long>(now_ms - compose_ok_since_ms_));
      enter(State::Capture, now_ms);
      return;
    }
  } else if (sweep_step_ < kSweepLen && !head_.faulted()) {
    // 顔判定が無いので、動作確認用に小さく左右を見るだけ (step1 design §4)。
    if (head_.nudge(kSweep[sweep_step_] * config::HEAD_STEP_MAX, 0, now_ms)) {
      ++sweep_step_;
    }
  }

  if (now_ms - state_since_ms_ >= config::COMPOSE_TIMEOUT_MS) {
    if (!judged_ && sweep_step_ < kSweepLen && !head_.faulted()) {
      // 首振りが終わらないまま時間切れなら正面に戻してから撮る。
      ESP_LOGW(TAG, "compose sweep incomplete (%u/%u); back to neutral", sweep_step_, kSweepLen);
      head_.neutral(now_ms);
    }
    ESP_LOGI(TAG, "compose timeout");
    enter(State::Capture, now_ms);
  }
}

bool App::handleResult(bool capture, uint32_t now_ms) {
  edge::FrameResult r;
  if (!edge_.pollResult(r) || !r.valid || r.dropped) {
    return false;
  }

  // 首: edge の値は「希望」。nudge() が可動域・1 回の上限・間隔でクランプする。
  // 前回首を動かす前に撮ったフレームへの指示は、もう古いので使わない。
  if ((r.servo_dx != 0 || r.servo_dy != 0) && !head_.faulted() &&
      r.frame_id > nudge_after_frame_id_) {
    if (head_.nudge(r.servo_dx, r.servo_dy, now_ms)) {
      nudge_after_frame_id_ = last_offered_frame_id_;
    }
  }

  showHint(r.hint, capture);

  if (!capture) {
    const bool ok = r.face_count >= 1 && r.all_in_frame;
    if (ok && !compose_ok_) {
      compose_ok_ = true;
      compose_ok_since_ms_ = now_ms;
    } else if (!ok) {
      compose_ok_ = false;
    }
    return false;
  }

  if (r.face_count != shown_faces_ || r.target_face_count != shown_target_) {
    shown_faces_ = r.face_count;
    shown_target_ = r.target_face_count;
    ui::drawCaptureOverlay(shown_remaining_sec_, shown_faces_, shown_target_);
  }
  return r.accepted;
}

const char* App::bandText(edge::Hint hint) const {
  switch (hint) {
    case edge::Hint::Closer:  return kBandCloser;
    case edge::Hint::TooMany: return too_many_text_;
    case edge::Hint::None:    break;
  }
  return ui::kBandDefault;
}

void App::showHint(edge::Hint hint, bool capture) {
  if (hint == shown_hint_) {
    return;
  }
  shown_hint_ = hint;
  if (!capture) {
    ui::drawComposeOverlay(bandText(hint));
  } else {
    // CAPTURE は hint があるときだけ帯を出す。消すときは次のフレームが上書きする。
    capture_band_ = hint != edge::Hint::None;
    if (capture_band_) {
      ui::drawCaptureBand(bandText(hint));
    }
  }
  if (hint == edge::Hint::Closer && !closer_played_) {
    closer_played_ = true;
    audio_.playCloser();
  }
}

// ---- CAPTURE -------------------------------------------------------------

void App::updateCapture(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - state_since_ms_;
  const uint32_t total_ms = config::COUNTDOWN_SEC * 1000;
  if (elapsed >= total_ms) {
    ESP_LOGI(TAG, "capture timeout: %lu ms, device candidate frame_id=%lu",
             static_cast<unsigned long>(elapsed),
             static_cast<unsigned long>(candidate_.valid() ? candidate_.frameId() : 0));
    review_waiting_ = false;
    if (judged_) {
      // 10 秒経過後に届いた結果は採用しない (spec §6.2)。ここで捨てる。
      edge::FrameResult late;
      if (edge_.pollResult(late) && late.valid && late.accepted) {
        ESP_LOGW(TAG, "ignore accepted frame_id=%lu after %lu ms",
                 static_cast<unsigned long>(late.frame_id), static_cast<unsigned long>(elapsed));
      }
      edge_.sessionTimeout(session_);
      review_waiting_ = true;
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
    ui::drawCaptureOverlay(remaining, shown_faces_, shown_target_);
  }

  if (!judged_) {
    return;
  }
  if (!edge_.isOnline()) {
    failEdge(now_ms, kEdgeLost);
    return;
  }
  if (handleResult(true, now_ms)) {
    ESP_LOGI(TAG, "frame accepted by edge at %lu ms (faces %d/%d)",
             static_cast<unsigned long>(elapsed), shown_faces_, shown_target_);
    // accepted でも save を送る (edge は自動ではアップロードしない、protocol.md)。
    edge_.reviewDecision(session_, true);
    uploading_captured_ = true;
    enter(State::Uploading, now_ms);
  }
}

void App::resetFrameWatch(uint32_t now_ms) {
  last_frame_ms_ = now_ms;
  grab_failures_ = 0;
}

bool App::previewFrame(bool capture, uint32_t now_ms) {
  if (!camera_.ready()) {
    return true;  // プレビュー枠は状態に入ったときに描いてある
  }
  camera_fb_t* fb = camera_.grab();
  if (fb == nullptr) {
    ++grab_failures_;
    const uint32_t stalled = now_ms - last_frame_ms_;
    if (stalled >= kFrameStallMs) {
      ESP_LOGE(TAG, "no camera frame for %lu ms in %s (%lu grab failures)",
               static_cast<unsigned long>(stalled), stateName(state_),
               static_cast<unsigned long>(grab_failures_));
      if (session_.active && judged_) {
        edge_.sessionCancel(session_);
      }
      session_.end();
      error_kind_ = ErrorKind::Camera;
      error_reason_ = kCameraNoFrame;
      enter(State::Error, now_ms);
      return false;
    }
    return true;
  }
  last_frame_ms_ = now_ms;
  const uint32_t frame_id = session_.nextFrameId();
  const int16_t w = static_cast<int16_t>(fb->width);
  const int16_t h = static_cast<int16_t>(fb->height);
  const uint16_t* px = reinterpret_cast<const uint16_t*>(fb->buf);
  const bool moving = head_.isMoving();
  if (capture) {
    ui::drawCaptureFrame(px, w, h, capture_band_);
    // 首が動いている間のフレームは候補にしない (spec §4)。
    // edge の候補 JPEG が取れないときの予備として device でも 1 枚持つ。
    if (!moving) {
      candidate_.assign(*fb, frame_id);
    }
  } else {
    ui::drawComposeFrame(px, w, h);
  }
  // edge の顔判定。首が動いている間は送らない (ブレたフレームを判定・採用しない)。
  // offerFrame() はスロットへコピーして即 return する (直後に release() で fb を返すため)。
  if (judged_ && !moving) {
    if (edge_.offerFrame(session_, *fb, head_.targetX(), head_.targetY(),
                         capture ? edge::Phase::Capture : edge::Phase::Compose)) {
      last_offered_frame_id_ = frame_id;
      ++offered_frames_;
    }
  }
  camera_.release(fb);
  ++preview_frames_;
  return true;
}

void App::logPreviewStats(uint32_t now_ms) {
  const uint32_t dt = now_ms - preview_since_ms_;
  if (dt == 0) {
    return;
  }
  const float fps = preview_frames_ * 1000.0f / dt;
  ESP_LOGI(TAG, "preview %lu frames in %lu ms (%.1f fps), offered to edge %lu, head target x=%d y=%d",
           static_cast<unsigned long>(preview_frames_), static_cast<unsigned long>(dt), fps,
           static_cast<unsigned long>(offered_frames_), head_.targetX(), head_.targetY());
}

// ---- REVIEW --------------------------------------------------------------

int App::buttonHit(const hal::Event& ev, int count) const {
  if (ev.kind != hal::Event::Kind::ScreenTap) {
    return -1;
  }
  return ui::hitButton(ev.x, ev.y, count);
}

void App::drawDeviceReview() {
  if (candidate_.valid()) {
    ESP_LOGI(TAG, "review device candidate frame_id=%lu (%ux%u, %u bytes)",
             static_cast<unsigned long>(candidate_.frameId()), candidate_.width(),
             candidate_.height(), static_cast<unsigned>(candidate_.length()));
    ui::drawReview(stateTitle(State::Review), candidate_.pixels(), candidate_.width(),
                   candidate_.height());
    review_buttons_ = ui::kReviewButtons;
  } else {
    ESP_LOGW(TAG, "review without candidate");
    ui::drawReview(stateTitle(State::Review), nullptr, 0, 0);
    review_buttons_ = ui::kReviewButtonsNoCandidate;
  }
}

void App::showReview(bool edge_has_candidate) {
  if (!edge_has_candidate) {
    // edge が顔の無い時間切れと判断した: 保存できないので「撮り直す」だけ。
    ESP_LOGI(TAG, "review: edge has no candidate");
    ui::drawReview(stateTitle(State::Review), nullptr, 0, 0);
    review_buttons_ = ui::kReviewButtonsNoCandidate;
    return;
  }
  uint8_t* jpeg = nullptr;
  size_t len = 0;
  if (edge_.fetchCandidate(session_, jpeg, len)) {
    const bool drawn = ui::drawReviewJpeg(stateTitle(State::Review), jpeg, len);
    heap_caps_free(jpeg);
    if (drawn) {
      ESP_LOGI(TAG, "review: edge candidate jpeg (%u bytes)", static_cast<unsigned>(len));
      review_buttons_ = ui::kReviewButtons;
      return;
    }
  }
  // edge の候補を出せない。保存すると edge の候補が保存されるが、画面は device の保持フレーム。
  ESP_LOGW(TAG, "review: edge candidate unavailable (%s); show device frame", edge_.lastError());
  drawDeviceReview();
}

void App::updateReview(const hal::Event& ev, uint32_t now_ms) {
  if (review_waiting_) {
    bool ok = false;
    bool has_candidate = false;
    if (edge_.pollTimeout(ok, has_candidate)) {
      review_waiting_ = false;
      if (!ok) {
        // 通信失敗・edge 再起動後の 404 など。「候補なし」と区別して撮影を止める。
        failEdge(now_ms, kEdgeLost);
        return;
      }
      showReview(has_candidate);
    } else if (now_ms - state_since_ms_ >= kReviewWaitMs) {
      review_waiting_ = false;
      failEdge(now_ms, kEdgeLost);
    }
    return;  // 待っている間のタッチは無視 (ボタンは出ていない)
  }

  const int hit = buttonHit(ev, review_buttons_);
  if (review_buttons_ == ui::kReviewButtons && hit == 0) {  // 保存する
    ESP_LOGI(TAG, "review: save");
    if (judged_) {
      edge_.reviewDecision(session_, true);
    }
    uploading_captured_ = false;
    enter(State::Uploading, now_ms);
  } else if ((review_buttons_ == ui::kReviewButtons && hit == 1) ||
             (review_buttons_ == ui::kReviewButtonsNoCandidate && hit == 0)) {  // 撮り直す
    ESP_LOGI(TAG, "review: retake");
    if (judged_) {
      edge_.reviewDecision(session_, false);
    }
    retake(now_ms);
  }
}

// ---- UPLOADING -----------------------------------------------------------

void App::updateUploading(uint32_t now_ms) {
  if (!photo_ready_) {
    if (!kEdgeEnabled) {
      // PHOTOBOOTH_NO_EDGE: ステップ1と同じ設定ファイルの固定 URL。削除時刻は未定。
      photo_url_ = config::FIXED_PHOTO_URL;
      share_url_ = config::FIXED_SHARE_URL;
      expires_at_ = "--:--";
      photo_ready_ = true;
    } else if (!judged_) {
      // 「判定なしで撮影」は edge に写真が無いので保存できない (QR を捏造しない、spec §9)。
      error_kind_ = ErrorKind::NoPc;
      error_reason_ = kNoPc;
      enter(State::Error, now_ms);
      return;
    } else {
      edge::PhotoInfo info;
      if (edge_.pollPhotoReady(info)) {
        if (info.status != edge::PhotoInfo::Status::Ready) {
          failUpload(now_ms, info.reason[0] ? info.reason : "error");
          return;
        }
        photo_url_ = info.photo_url;
        share_url_ = info.share_url;
        expires_at_ = formatExpires(info.expires_at);
        photo_ready_ = true;
      } else if (now_ms - state_since_ms_ >= config::UPLOAD_WAIT_MS) {
        failUpload(now_ms, "timeout");
        return;
      }
    }
    if (photo_ready_) {
      ESP_LOGI(TAG, "photo ready (expires %s) in %lu ms", expires_at_.c_str(),
               static_cast<unsigned long>(now_ms - state_since_ms_));  // URL/トークンはログに出さない
    }
  }
  // 「撮れたよ」を言い切ってから QR に進む。
  const bool sound_done =
      !audio_.isPlaying() || now_ms - state_since_ms_ > kUploadingSoundGuardMs;
  if (photo_ready_ && sound_done) {
    enter(State::PhotoQr, now_ms);
  }
}

// ---- PHOTO_QR / X_QR -----------------------------------------------------

void App::updatePhotoQr(const hal::Event& ev, uint32_t now_ms) {
  const int hit = buttonHit(ev, ui::kPhotoQrButtons);
  if (hit == 0) {  // 次へ
    enter(State::XQr, now_ms);
  } else if (hit == 1) {  // 撮り直す
    ESP_LOGI(TAG, "photo qr: retake");
    retake(now_ms);
  }
}

void App::updateXQr(const hal::Event& ev, uint32_t now_ms) {
  const int hit = buttonHit(ev, ui::kXQrButtons);
  if (hit == 0) {  // 戻る
    enter(State::PhotoQr, now_ms);
  } else if (hit == 1) {  // 終了
    ESP_LOGI(TAG, "session end id=%s", session_.id);
    enter(State::Idle, now_ms);
  }
}

// ---- ERROR ---------------------------------------------------------------

void App::updateError(const hal::Event& ev, uint32_t now_ms) {
  const int hit = buttonHit(ev, ui::kErrorButtons);
  if (hit == 0) {  // 再試行
    ESP_LOGI(TAG, "error: retry (%s)", error_reason_);
    switch (error_kind_) {
      case ErrorKind::Camera:
        // 初期化失敗・フレーム停止のどちらでも、ドライバを破棄してから初期化し直す。
        if (camera_.restart()) {
          enter(State::Idle, now_ms);
        } else {
          ESP_LOGE(TAG, "retry failed: camera err=0x%x", camera_.lastError());
          ui::drawError(stateTitle(state_), error_reason_);
        }
        break;

      case ErrorKind::Upload:
        if (upload_retries_ >= config::UPLOAD_RETRY) {
          ESP_LOGW(TAG, "upload retry limit (%u) reached", static_cast<unsigned>(config::UPLOAD_RETRY));
          break;  // 画面には「終了して撮り直してね」を出してある
        }
        ++upload_retries_;
        edge_.reviewDecision(session_, true);  // 同じ session_id で save を送り直す (edge は冪等)
        uploading_captured_ = false;
        uploading_quiet_ = true;
        enter(State::Uploading, now_ms);
        break;

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
  } else if (hit == 1) {  // 終了
    if (session_.active && judged_) {
      edge_.sessionCancel(session_);
    }
    enter(State::Idle, now_ms);
  }
}

// ---- DIAG ----------------------------------------------------------------

void App::buildDiag(char* out, size_t len) {
  edge::Diagnostics d;
  edge_.diagnostics(d);
  const char* wifi = d.wifi_connected ? "接続済み" : (d.wifi_connecting ? "接続中" : "未接続");
  char rssi[16];
  if (d.wifi_connected) {
    snprintf(rssi, sizeof(rssi), "%d dBm", d.rssi);
  } else {
    snprintf(rssi, sizeof(rssi), "-");
  }
  snprintf(out, len,
           "SSID: %s (%s)\nIP: %s\nRSSI: %s\nPC: %s:%u (%s)\nエラー: %s",
           d.ssid, wifi, d.ip[0] ? d.ip : "-", rssi, d.edge_host,
           static_cast<unsigned>(d.edge_port), d.online ? "応答あり" : "応答なし",
           d.last_error[0] ? d.last_error : "なし");
}

void App::updateDiag(const hal::Event& ev, uint32_t now_ms) {
  if (ev.kind == hal::Event::Kind::HeadTap) {  // 戻る (ボタン帯は 2 つまで、design §4.4)
    enter(State::Idle, now_ms);
    return;
  }
  const int hit = buttonHit(ev, ui::kDiagButtons);
  if (hit == 0) {  // 再接続
    ESP_LOGI(TAG, "diag: reconnect");
    edge_.reconnect();
  } else if (hit == 1) {  // 判定なしで撮影
    ESP_LOGI(TAG, "diag: shoot without edge judge");
    if (!checkCamera(now_ms)) return;
    startSession(now_ms, false);
    enter(State::Announce, now_ms);
    return;
  }

  if (now_ms - diag_refreshed_ms_ >= kDiagRefreshMs || hit == 0) {
    diag_refreshed_ms_ = now_ms;
    char body[sizeof(diag_body_)];
    buildDiag(body, sizeof(body));
    if (strcmp(body, diag_body_) != 0) {
      memcpy(diag_body_, body, sizeof(diag_body_));
      ui::updateDiagBody(diag_body_);
    }
  }
}

}  // namespace app
