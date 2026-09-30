#include "hal/camera.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstring>

namespace hal {

namespace {
constexpr const char* TAG = "camera";

#ifndef PHOTOBOOTH_NO_CAMERA
// M5Stack 公式カメラ例 (StackChan) と同じ設定。
camera_config_t makeConfig() {
  camera_config_t c = {};
  c.pin_pwdn = -1;
  c.pin_reset = -1;
  c.pin_xclk = -1;
  c.pin_sccb_sda = 12;
  c.pin_sccb_scl = 11;
  c.pin_d7 = 47;
  c.pin_d6 = 48;
  c.pin_d5 = 16;
  c.pin_d4 = 15;
  c.pin_d3 = 42;
  c.pin_d2 = 41;
  c.pin_d1 = 40;
  c.pin_d0 = 39;
  c.pin_vsync = 46;
  c.pin_href = 38;
  c.pin_pclk = 45;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_RGB565;
  c.frame_size = FRAMESIZE_QVGA;
  c.jpeg_quality = 0;
  c.fb_count = 2;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  c.sccb_i2c_port = -1;
  return c;
}
#endif
}  // namespace

bool Camera::begin() {
#ifdef PHOTOBOOTH_NO_CAMERA
  ESP_LOGW(TAG, "PHOTOBOOTH_NO_CAMERA: camera init skipped");
  ready_ = false;
  last_error_ = 0;
  return true;
#else
  if (ready_) {
    return true;
  }
  const uint32_t t0 = millis();
  // カメラ SCCB は CoreS3 内部 I2C (GPIO 12/11) と同じピン。M5 側の I2C を先に手放す。
  M5.In_I2C.release();
  const camera_config_t config = makeConfig();
  const esp_err_t err = esp_camera_init(&config);
  last_error_ = err;
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_camera_init failed: 0x%x (%s)", err, esp_err_to_name(err));
    // 途中まで確保したドライバ資源を片付け、「再試行」で最初から初期化できるようにする。
    // 何も確保されていなければ ESP_ERR_INVALID_STATE が返るだけなので戻り値は見ない。
    (void)esp_camera_deinit();
    ready_ = false;
    return false;
  }
  ready_ = true;
  ESP_LOGI(TAG, "camera ready (%lu ms)", static_cast<unsigned long>(millis() - t0));
  return true;
#endif
}

camera_fb_t* Camera::grab() {
  if (!ready_) {
    return nullptr;
  }
  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    ESP_LOGW(TAG, "esp_camera_fb_get returned null");
  }
  return fb;
}

void Camera::release(camera_fb_t* fb) {
  if (fb != nullptr) {
    esp_camera_fb_return(fb);
  }
}

bool FrameCopy::assign(const camera_fb_t& fb, uint32_t frame_id) {
  if (fb.buf == nullptr || fb.len == 0) {
    return false;
  }
  if (buf_ == nullptr || capacity_ < fb.len) {
    clear();
    buf_ = static_cast<uint8_t*>(heap_caps_malloc(fb.len, MALLOC_CAP_SPIRAM));
    if (buf_ == nullptr) {
      ESP_LOGE(TAG, "candidate alloc failed (%u bytes, free PSRAM %u)",
               static_cast<unsigned>(fb.len),
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      return false;
    }
    capacity_ = fb.len;
  }
  memcpy(buf_, fb.buf, fb.len);
  len_ = fb.len;
  width_ = static_cast<uint16_t>(fb.width);
  height_ = static_cast<uint16_t>(fb.height);
  frame_id_ = frame_id;
  return true;
}

void FrameCopy::clear() {
  if (buf_ != nullptr) {
    heap_caps_free(buf_);
  }
  buf_ = nullptr;
  capacity_ = 0;
  len_ = 0;
  width_ = 0;
  height_ = 0;
  frame_id_ = 0;
}

}  // namespace hal
