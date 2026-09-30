#include "hal/head.h"

#include <M5StackChan.h>
#include <esp_log.h>

#include <algorithm>

#include "config.h"

namespace hal {

namespace {
constexpr const char* TAG = "head";
// isMoving() は UART でサーボに問い合わせるので間引く。
constexpr uint32_t kPollIntervalMs = 100;

int clampX(int x) { return std::min(std::max(x, config::HEAD_X_MIN), config::HEAD_X_MAX); }
int clampY(int y) { return std::min(std::max(y, config::HEAD_Y_MIN), config::HEAD_Y_MAX); }
}  // namespace

void Head::begin(uint32_t now_ms) {
  static_assert(config::HEAD_X_MIN <= config::HEAD_X_NEUTRAL &&
                    config::HEAD_X_NEUTRAL <= config::HEAD_X_MAX,
                "HEAD_X_NEUTRAL must be within [HEAD_X_MIN, HEAD_X_MAX]");
  static_assert(config::HEAD_Y_MIN <= config::HEAD_Y_NEUTRAL &&
                    config::HEAD_Y_NEUTRAL <= config::HEAD_Y_MAX,
                "HEAD_Y_NEUTRAL must be within [HEAD_Y_MIN, HEAD_Y_MAX]");
  // BSP の可動域 (yaw -1280..1280, pitch 0..900) と公式推奨の Y 5〜85° を越えない。
  static_assert(config::HEAD_X_MIN >= -1280 && config::HEAD_X_MAX <= 1280, "HEAD_X out of BSP range");
  static_assert(config::HEAD_Y_MIN >= 50 && config::HEAD_Y_MAX <= 850, "HEAD_Y out of 5..85 deg");
  static_assert(config::HEAD_STEP_MAX > 0, "HEAD_STEP_MAX must be positive");

  faulted_ = false;
  ESP_LOGI(TAG, "begin: current x=%d y=%d", readCurrentX(), readCurrentY());
  neutral(now_ms);
}

void Head::update(uint32_t now_ms) {
  // 指示してから止まるまでの間だけ問い合わせる。止まった後の一時的な読み取り失敗
  // (ReadMove が -1 を返すと「動作中」に見える) で異常判定しないため。
  if (faulted_ || !watching_) {
    return;
  }
  if (now_ms - last_poll_ms_ < kPollIntervalMs) {
    return;
  }
  last_poll_ms_ = now_ms;
  moving_ = M5StackChan.Motion.isMoving();
  if (!moving_) {
    watching_ = false;
    ESP_LOGI(TAG, "settled at target x=%d y=%d in %lu ms", target_x_, target_y_,
             static_cast<unsigned long>(now_ms - last_cmd_ms_));
    return;
  }
  if (now_ms - last_cmd_ms_ > config::HEAD_MOVE_TIMEOUT_MS) {
    faulted_ = true;
    watching_ = false;
    moving_ = false;
    M5StackChan.Motion.stop();
    ESP_LOGE(TAG, "servo not settling %lu ms after command (target x=%d y=%d); head disabled",
             static_cast<unsigned long>(now_ms - last_cmd_ms_), target_x_, target_y_);
  }
}

void Head::command(int x, int y, uint32_t now_ms) {
  target_x_ = clampX(x);
  target_y_ = clampY(y);
  last_cmd_ms_ = now_ms;
  last_poll_ms_ = now_ms;
  has_cmd_ = true;
  if (faulted_) {
    return;
  }
  moving_ = true;  // 指示直後は動作中とみなす (次の update() で実測に置き換わる)
  watching_ = true;
  M5StackChan.Motion.move(target_x_, target_y_, config::HEAD_SPEED);
}

void Head::moveTo(int x, int y, uint32_t now_ms) {
  const int cx = clampX(x);
  const int cy = clampY(y);
  if (cx != x || cy != y) {
    ESP_LOGW(TAG, "moveTo clamped (%d,%d) -> (%d,%d)", x, y, cx, cy);
  }
  command(cx, cy, now_ms);
  ESP_LOGI(TAG, "moveTo x=%d y=%d", target_x_, target_y_);
}

bool Head::nudge(int dx, int dy, uint32_t now_ms) {
  if (has_cmd_ && now_ms - last_cmd_ms_ < config::HEAD_STEP_INTERVAL_MS) {
    return false;
  }
  dx = std::min(std::max(dx, -config::HEAD_STEP_MAX), config::HEAD_STEP_MAX);
  dy = std::min(std::max(dy, -config::HEAD_STEP_MAX), config::HEAD_STEP_MAX);
  const int prev_x = target_x_;
  const int prev_y = target_y_;
  command(target_x_ + dx, target_y_ + dy, now_ms);
  ESP_LOGI(TAG, "nudge d=(%d,%d) target (%d,%d) -> (%d,%d)", dx, dy, prev_x, prev_y, target_x_,
           target_y_);
  return true;
}

void Head::neutral(uint32_t now_ms) {
  moveTo(config::HEAD_X_NEUTRAL, config::HEAD_Y_NEUTRAL, now_ms);
}

int Head::readCurrentX() { return M5StackChan.Motion.getCurrentYawAngle(); }
int Head::readCurrentY() { return M5StackChan.Motion.getCurrentPitchAngle(); }

}  // namespace hal
