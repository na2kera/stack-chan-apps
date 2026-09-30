// edge (PC) との通信の抽象インターフェース。
// イベント名は docs/spec.md §8 と一致させる。ステップ2で実装が入る。
#pragma once

#include <Arduino.h>
#include <esp_camera.h>

#include <cstdint>

#include "app/session.h"

namespace edge {

// frame_result (spec §8)。
struct FrameResult {
  bool valid;
  uint8_t face_count;
  uint8_t target_face_count;
  bool all_eyes_open;
  bool all_smiling;
  int servo_dx;
  int servo_dy;
  bool accepted;
};

class EdgeClient {
 public:
  virtual ~EdgeClient() = default;
  virtual bool isOnline() = 0;
  virtual void sessionStart(const app::Session&) = 0;
  virtual bool sendFrame(const app::Session&, const camera_fb_t&) = 0;  // 非同期。結果は pollResult
  virtual bool pollResult(FrameResult& out) = 0;
  virtual void sessionTimeout(const app::Session&) = 0;
  virtual void reviewDecision(const app::Session&, bool save) = 0;
  virtual bool pollPhotoReady(String& photo_url, String& share_url, String& expires_at) = 0;
  virtual void sessionCancel(const app::Session&) = 0;
};

}  // namespace edge
