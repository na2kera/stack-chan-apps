// 撮影フローの状態機械 (docs/design/step1-device.md §4)。
//
// 入力イベントと時刻 (millis) だけで遷移する。描画は ui、ハードウェアは hal、
// edge 通信は EdgeClient を呼ぶだけで、M5 / M5StackChan を直接触らない。
#pragma once

#include <Arduino.h>

#include <cstdint>

#include "app/session.h"
#include "app/state.h"
#include "edge/edge_client.h"
#include "hal/audio.h"
#include "hal/camera.h"
#include "hal/head.h"
#include "hal/input.h"

namespace app {

class App {
 public:
  App(hal::Camera& camera, hal::Head& head, hal::Audio& audio, edge::EdgeClient& edge)
      : camera_(camera), head_(head), audio_(audio), edge_(edge) {}

  // camera_ok=false なら ERROR (カメラ初期化失敗) から始める。
  void begin(uint32_t now_ms, bool camera_ok);
  void update(const hal::Event& ev, uint32_t now_ms);

  State state() const { return state_; }

 private:
  void enter(State next, uint32_t now_ms);
  void startSession(uint32_t now_ms);

  void updateIdle(const hal::Event& ev, uint32_t now_ms);
  void updateAnnounce(uint32_t now_ms);
  void updateCompose(uint32_t now_ms);
  void updateCapture(uint32_t now_ms);
  void updateReview(const hal::Event& ev, uint32_t now_ms);
  void updateUploading(uint32_t now_ms);
  void updatePhotoQr(const hal::Event& ev, uint32_t now_ms);
  void updateXQr(const hal::Event& ev, uint32_t now_ms);
  void updateError(const hal::Event& ev, uint32_t now_ms);

  // プレビュー 1 フレーム分。CAPTURE なら条件を満たすフレームを候補として保持する。
  void previewFrame(bool capture, uint32_t now_ms);
  void logPreviewStats(uint32_t now_ms);
  void drawIdle(uint32_t now_ms);
  const char* idleWarning() const;
  int buttonHit(const hal::Event& ev, int count) const;

  hal::Camera& camera_;
  hal::Head& head_;
  hal::Audio& audio_;
  edge::EdgeClient& edge_;

  State state_ = State::Idle;
  uint32_t state_since_ms_ = 0;
  Session session_;

  // IDLE
  bool eyes_open_ = true;
  uint32_t eyes_changed_ms_ = 0;
  bool idle_head_fault_drawn_ = false;  // IDLE 画面に首の警告を描いたか

  // COMPOSE の首振りシーケンスの進み
  uint8_t sweep_step_ = 0;

  // CAPTURE
  int shown_remaining_sec_ = -1;

  // プレビューの実測
  uint32_t preview_frames_ = 0;
  uint32_t preview_since_ms_ = 0;

  // 候補フレーム (REVIEW で表示、撮り直し・終了で解放)
  hal::FrameCopy candidate_;

  // PHOTO_QR / X_QR に出す URL
  String photo_url_;
  String share_url_;
  String expires_at_;
  bool photo_ready_ = false;

  const char* error_reason_ = "";
};

}  // namespace app
