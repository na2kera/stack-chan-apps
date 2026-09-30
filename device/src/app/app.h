// 撮影フローの状態機械 (docs/design/step1-device.md §4, step2b-device-edge.md §4.3)。
//
// 入力イベントと時刻 (millis) だけで遷移する。描画は ui、ハードウェアは hal、
// edge 通信は EdgeClient を呼ぶだけで、M5 / M5StackChan / WiFi / HTTP を直接触らない。
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
  // ERROR の理由の種類。「再試行」の動作が変わる。
  enum class ErrorKind : uint8_t {
    Camera,  // カメラ初期化失敗・フレーム停止 → カメラを初期化し直す
    Edge,    // 撮影中に edge との通信が切れた → 繋がっていれば新しいセッション、無ければ DIAG
    Upload,  // 写真を保存できない → save を送り直す (UPLOAD_RETRY 回まで)
    NoPc,    // 判定なしで撮影した写真は保存できない → Edge と同じ
  };

  void enter(State next, uint32_t now_ms);
  // judged=true なら edge の顔判定つき (sessionStart を送る)。false は固定フロー。
  void startSession(uint32_t now_ms, bool judged);
  // 撮り直し。判定つきで edge が切れていれば ERROR。
  void retake(uint32_t now_ms);
  // 撮影開始の前にカメラを確認する。使えなければ ERROR へ進めて false。
  bool checkCamera(uint32_t now_ms);
  // edge との通信が切れたので撮影を止めて ERROR へ。
  void failEdge(uint32_t now_ms, const char* what);
  void failUpload(uint32_t now_ms, const char* reason);

  void updateIdle(const hal::Event& ev, uint32_t now_ms);
  void updateAnnounce(uint32_t now_ms);
  void updateCompose(uint32_t now_ms);
  void updateCapture(uint32_t now_ms);
  void updateReview(const hal::Event& ev, uint32_t now_ms);
  void updateUploading(uint32_t now_ms);
  void updatePhotoQr(const hal::Event& ev, uint32_t now_ms);
  void updateXQr(const hal::Event& ev, uint32_t now_ms);
  void updateError(const hal::Event& ev, uint32_t now_ms);
  void updateDiag(const hal::Event& ev, uint32_t now_ms);

  // プレビュー 1 フレーム分。CAPTURE なら条件を満たすフレームを候補として保持する。
  // 判定つきなら首が止まっているときだけ edge にフレームを渡す。
  // フレームが kFrameStallMs 続けて取れなければ ERROR に遷移して false を返す。
  bool previewFrame(bool capture, uint32_t now_ms);
  // edge の frame_result を 1 件処理する (首・案内帯・人数・COMPOSE の安定判定)。
  // CAPTURE で accepted なら true。
  bool handleResult(bool capture, uint32_t now_ms);
  void showHint(edge::Hint hint, bool capture);
  const char* bandText(edge::Hint hint) const;
  // REVIEW: edge の候補 (JPEG) か device の保持フレームを出す。
  void showReview(bool edge_has_candidate);
  void drawDeviceReview();
  void resetFrameWatch(uint32_t now_ms);
  void logPreviewStats(uint32_t now_ms);
  void drawIdle(uint32_t now_ms);
  const char* idleWarning() const;
  void buildDiag(char* out, size_t len);
  int buttonHit(const hal::Event& ev, int count) const;

  hal::Camera& camera_;
  hal::Head& head_;
  hal::Audio& audio_;
  edge::EdgeClient& edge_;

  State state_ = State::Idle;
  uint32_t state_since_ms_ = 0;
  Session session_;
  bool judged_ = false;  // 今のセッションは edge の判定つきか

  // IDLE
  bool eyes_open_ = true;
  uint32_t eyes_changed_ms_ = 0;
  bool idle_head_fault_drawn_ = false;  // IDLE 画面に首の警告を描いたか
  bool idle_online_drawn_ = false;      // IDLE 画面に描いた接続表示

  // COMPOSE の首振りシーケンスの進み (判定なしのときだけ)
  uint8_t sweep_step_ = 0;
  // COMPOSE: 「1 人以上が枠内」が続いている間 true
  bool compose_ok_ = false;
  uint32_t compose_ok_since_ms_ = 0;

  // COMPOSE / CAPTURE の案内帯と人数表示
  edge::Hint shown_hint_ = edge::Hint::None;
  bool capture_band_ = false;   // CAPTURE で案内帯を出しているか
  bool closer_played_ = false;  // closer.wav はセッションで 1 回だけ
  int shown_faces_ = -1;
  int shown_target_ = 0;
  char too_many_text_[32] = {};

  // 首: この frame_id 以前のフレームへの servo_dx/dy は、首を動かす前の画像なので使わない
  uint32_t last_offered_frame_id_ = 0;
  uint32_t nudge_after_frame_id_ = 0;

  // CAPTURE
  int shown_remaining_sec_ = -1;

  // REVIEW
  bool review_waiting_ = false;  // session_timeout の応答待ち
  int review_buttons_ = 0;       // 描いたボタン数 (2 = 保存/撮り直し、1 = 撮り直しのみ)

  // UPLOADING
  bool uploading_captured_ = false;  // 自動採用 (「撮れたよ」を出す)
  bool uploading_quiet_ = false;     // 再試行なので captured.wav を鳴らさない
  uint8_t upload_retries_ = 0;

  // プレビューの実測
  uint32_t preview_frames_ = 0;
  uint32_t offered_frames_ = 0;
  uint32_t preview_since_ms_ = 0;
  // フレーム取得の監視 (COMPOSE / CAPTURE に入るたびにリセット)
  uint32_t last_frame_ms_ = 0;
  uint32_t grab_failures_ = 0;

  // 候補フレーム (REVIEW で表示、撮り直し・終了で解放)
  hal::FrameCopy candidate_;

  // PHOTO_QR / X_QR に出す URL
  String photo_url_;
  String share_url_;
  String expires_at_;  // 表示用 "HH:MM"
  bool photo_ready_ = false;

  // ERROR
  ErrorKind error_kind_ = ErrorKind::Camera;
  const char* error_reason_ = "";
  char error_buf_[192] = {};

  // DIAG
  char diag_body_[320] = {};
  uint32_t diag_refreshed_ms_ = 0;
};

}  // namespace app
