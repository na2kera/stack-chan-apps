// 首サーボのラッパ (docs/design/step1-device.md §5 head)。
//
// 角度の単位は BSP と同じ 1/10 度。X = yaw、Y = pitch (0 = 下向き, 900 = 上向き)。
// 可動域・neutral・ステップ上限は config::HEAD_* だけで決め、コードに固定値を持たない。
#pragma once

#include <cstdint>

namespace hal {

class Head {
 public:
  // neutral() を指示し、以後 update() で応答を監視する。
  void begin(uint32_t now_ms);

  // 監視処理。loop ごとに呼ぶ。指示から停止までの間だけ isMoving() を問い合わせてキャッシュし、
  // 指示後 HEAD_MOVE_TIMEOUT_MS を過ぎても動き続けていればサーボ異常とみなす。
  void update(uint32_t now_ms);

  // クランプしてから Motion.move(x, y, HEAD_SPEED)。
  void moveTo(int x, int y, uint32_t now_ms);

  // 1 回の変化量を ±HEAD_STEP_MAX に制限して相対移動する。
  // 前回指示から HEAD_STEP_INTERVAL_MS 未満なら何もせず false。
  bool nudge(int dx, int dy, uint32_t now_ms);

  // (HEAD_X_NEUTRAL, HEAD_Y_NEUTRAL) へ。BSP の goHome() は (0, 0) = 下向きなので使わない。
  void neutral(uint32_t now_ms);

  // 動作中か (update() が取ったキャッシュ)。異常時は false (固定カメラ扱い)。
  bool isMoving() const { return !faulted_ && moving_; }

  // サーボ応答なしと判定済みか。以後は首を動かさない。
  bool faulted() const { return faulted_; }

  int targetX() const { return target_x_; }
  int targetY() const { return target_y_; }

  // BSP から現在角を読む (UART 通信が走るのでログ用途に限る)。
  int readCurrentX();
  int readCurrentY();

 private:
  void command(int x, int y, uint32_t now_ms);

  int target_x_ = 0;
  int target_y_ = 0;
  uint32_t last_cmd_ms_ = 0;
  bool has_cmd_ = false;
  bool moving_ = false;
  bool watching_ = false;  // 指示後、停止を確認するまで true
  bool faulted_ = false;
  uint32_t last_poll_ms_ = 0;
};

}  // namespace hal
