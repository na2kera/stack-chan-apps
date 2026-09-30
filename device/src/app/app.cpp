#include "app/app.h"

#include <esp_log.h>

#include "config.h"
#include "ui/screens.h"

namespace app {

namespace {

constexpr const char* TAG = "app";

// ANNOUNCE の保険。isPlaying() が落ちてこない場合でも固まらないようにする
// (announce.wav は 3 秒弱。調整値ではないので config には出さない)。
constexpr uint32_t kAnnounceGuardMs = 15000;
// UPLOADING の保険 (captured.wav の再生完了待ち)。
constexpr uint32_t kUploadingSoundGuardMs = 5000;

// COMPOSE の首振り: neutral → 左に 1 ステップ → 右に 1 ステップ → neutral。
// 1 回の指示は ±HEAD_STEP_MAX に制限されるので、neutral を挟んで 4 回の nudge で表す。
constexpr int kSweep[] = {-1, +1, +1, -1};
constexpr uint8_t kSweepLen = sizeof(kSweep) / sizeof(kSweep[0]);

constexpr const char* kCameraInitFailed = "カメラ初期化失敗";
constexpr const char* kCameraNoFrame = "カメラからフレームを取得できません";

// COMPOSE / CAPTURE でこの時間フレームが 1 枚も取れなければカメラ停止とみなす。
constexpr uint32_t kFrameStallMs = 2000;

}  // namespace

void App::begin(uint32_t now_ms, bool camera_ok) {
  ui::begin();
  state_since_ms_ = now_ms;
  if (!camera_ok) {
    error_reason_ = kCameraInitFailed;
    state_ = State::Error;
    ESP_LOGW(TAG, "start in %s: %s", stateName(state_), error_reason_);
    ui::drawError(stateTitle(state_), error_reason_);
    return;
  }
  state_ = State::Idle;
  ESP_LOGI(TAG, "start in %s", stateName(state_));
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
      preview_frames_ = 0;
      preview_since_ms_ = now_ms;
      resetFrameWatch(now_ms);
      head_.neutral(now_ms);
      if (!camera_.ready()) {
        ui::drawPreviewPlaceholder();
      }
      ui::drawComposeOverlay();
      break;

    case State::Capture:
      // CAPTURE の 10 秒はここを基準に測る (spec §4)。
      shown_remaining_sec_ = -1;
      preview_frames_ = 0;
      preview_since_ms_ = now_ms;
      resetFrameWatch(now_ms);
      candidate_.clear();
      if (!camera_.ready()) {
        ui::drawPreviewPlaceholder();
      }
      break;

    case State::Review:
      if (candidate_.valid()) {
        ESP_LOGI(TAG, "review candidate frame_id=%lu (%ux%u, %u bytes)",
                 static_cast<unsigned long>(candidate_.frameId()), candidate_.width(),
                 candidate_.height(), static_cast<unsigned>(candidate_.length()));
        ui::drawReview(stateTitle(next), candidate_.pixels(), candidate_.width(),
                       candidate_.height());
      } else {
        ESP_LOGW(TAG, "review without candidate");
        ui::drawReview(stateTitle(next), nullptr, 0, 0);
      }
      break;

    case State::Uploading:
      photo_ready_ = false;
      ui::drawUploading(stateTitle(next));
      audio_.playCaptured();
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
  }
}

void App::startSession(uint32_t now_ms) {
  candidate_.clear();
  session_.start(now_ms);
  ESP_LOGI(TAG, "session start id=%s", session_.id);
  if (edge_.isOnline()) {
    edge_.sessionStart(session_);
  }
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
  }
}

// ---- IDLE ----------------------------------------------------------------

void App::updateIdle(const hal::Event& ev, uint32_t now_ms) {
  // 起動指示 (タッチ) は IDLE でだけ受ける。画面はどこを触っても良い。
  if (ev.kind == hal::Event::Kind::ScreenTap || ev.kind == hal::Event::Kind::HeadTap) {
    ESP_LOGI(TAG, "start requested by %s",
             ev.kind == hal::Event::Kind::HeadTap ? "head touch" : "screen touch");
    if (!camera_.ready() && !hal::Camera::disabled()) {
      error_reason_ = kCameraInitFailed;
      enter(State::Error, now_ms);
      return;
    }
    startSession(now_ms);
    enter(State::Announce, now_ms);
    return;
  }

  // 起動直後の首の異常判定は数秒遅れて出るので、変わったら描き直す。
  if (head_.faulted() != idle_head_fault_drawn_) {
    drawIdle(now_ms);
    return;
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
  ui::drawIdle(edge_.isOnline(), idleWarning());
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

  // 顔判定が無いので、動作確認用に小さく左右を見るだけ (design §4)。
  if (sweep_step_ < kSweepLen && !head_.faulted()) {
    if (head_.nudge(kSweep[sweep_step_] * config::HEAD_STEP_MAX, 0, now_ms)) {
      ++sweep_step_;
    }
  }

  if (now_ms - state_since_ms_ >= config::COMPOSE_TIMEOUT_MS) {
    if (sweep_step_ < kSweepLen && !head_.faulted()) {
      // 首振りが終わらないまま時間切れなら正面に戻してから撮る。
      ESP_LOGW(TAG, "compose sweep incomplete (%u/%u); back to neutral", sweep_step_, kSweepLen);
      head_.neutral(now_ms);
    }
    enter(State::Capture, now_ms);
  }
}

// ---- CAPTURE -------------------------------------------------------------

void App::updateCapture(uint32_t now_ms) {
  const uint32_t elapsed = now_ms - state_since_ms_;
  const uint32_t total_ms = config::COUNTDOWN_SEC * 1000;
  if (elapsed >= total_ms) {
    ESP_LOGI(TAG, "capture timeout: %lu ms, candidate frame_id=%lu", static_cast<unsigned long>(elapsed),
             static_cast<unsigned long>(candidate_.valid() ? candidate_.frameId() : 0));
    if (edge_.isOnline()) {
      edge_.sessionTimeout(session_);
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
    ui::drawCaptureOverlay(remaining, -1);  // ステップ1は人数不明「--」
  }

  // edge が判定する経路 (ステップ2)。NullEdge は offline なのでここは通らない。
  if (edge_.isOnline()) {
    edge::FrameResult result{};
    if (edge_.pollResult(result) && result.valid && result.accepted) {
      ESP_LOGI(TAG, "frame accepted by edge (faces %u/%u)", result.face_count,
               result.target_face_count);
      enter(State::Uploading, now_ms);
    }
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
      if (session_.active && edge_.isOnline()) {
        edge_.sessionCancel(session_);
      }
      session_.end();
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
  if (capture) {
    ui::drawCaptureFrame(px, w, h);
    // 首が動いている間のフレームは候補にしない (spec §4)。
    if (!head_.isMoving()) {
      candidate_.assign(*fb, frame_id);
    }
  } else {
    ui::drawComposeFrame(px, w, h);
  }
  // edge の顔判定 (ステップ2)。NullEdge は offline なので送らない。
  // sendFrame() は return までに fb->buf を使い終える契約 (直後に release() で返すため)。
  if (edge_.isOnline()) {
    edge_.sendFrame(session_, *fb);
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
  ESP_LOGI(TAG, "preview %lu frames in %lu ms (%.1f fps), head target x=%d y=%d",
           static_cast<unsigned long>(preview_frames_), static_cast<unsigned long>(dt), fps,
           head_.targetX(), head_.targetY());
}

// ---- REVIEW --------------------------------------------------------------

int App::buttonHit(const hal::Event& ev, int count) const {
  if (ev.kind != hal::Event::Kind::ScreenTap) {
    return -1;
  }
  return ui::hitButton(ev.x, ev.y, count);
}

void App::updateReview(const hal::Event& ev, uint32_t now_ms) {
  if (candidate_.valid()) {
    const int hit = buttonHit(ev, ui::kReviewButtons);
    if (hit == 0) {  // 保存する
      ESP_LOGI(TAG, "review: save frame_id=%lu", static_cast<unsigned long>(candidate_.frameId()));
      if (edge_.isOnline()) {
        edge_.reviewDecision(session_, true);
      }
      enter(State::Uploading, now_ms);
    } else if (hit == 1) {  // 撮り直す
      ESP_LOGI(TAG, "review: retake");
      if (edge_.isOnline()) {
        edge_.reviewDecision(session_, false);
      }
      startSession(now_ms);
      enter(State::Announce, now_ms);
    }
  } else {
    if (buttonHit(ev, ui::kReviewButtonsNoCandidate) == 0) {  // 撮り直す
      ESP_LOGI(TAG, "review: retake (no candidate)");
      if (edge_.isOnline()) {
        edge_.reviewDecision(session_, false);
      }
      startSession(now_ms);
      enter(State::Announce, now_ms);
    }
  }
}

// ---- UPLOADING -----------------------------------------------------------

void App::updateUploading(uint32_t now_ms) {
  if (!photo_ready_) {
    if (edge_.isOnline()) {
      photo_ready_ = edge_.pollPhotoReady(photo_url_, share_url_, expires_at_);
    } else {
      // ステップ1: edge が無いので設定ファイルの固定 URL を使う。削除時刻は未定。
      photo_url_ = config::FIXED_PHOTO_URL;
      share_url_ = config::FIXED_SHARE_URL;
      expires_at_ = "--:--";
      photo_ready_ = true;
    }
    if (photo_ready_) {
      ESP_LOGI(TAG, "photo ready (expires %s)", expires_at_.c_str());  // URL/トークンはログに出さない
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
    startSession(now_ms);
    enter(State::Announce, now_ms);
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
    // 初期化失敗・フレーム停止のどちらでも、ドライバを破棄してから初期化し直す。
    if (camera_.restart()) {
      enter(State::Idle, now_ms);
    } else {
      ESP_LOGE(TAG, "retry failed: camera err=0x%x", camera_.lastError());
      ui::drawError(stateTitle(state_), error_reason_);
    }
  } else if (hit == 1) {  // 終了
    if (session_.active && edge_.isOnline()) {
      edge_.sessionCancel(session_);
    }
    enter(State::Idle, now_ms);
  }
}

}  // namespace app
