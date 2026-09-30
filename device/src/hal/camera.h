// GC0308 カメラのラッパ (docs/design/step1-device.md §5 camera)。
//
// ビルドフラグ PHOTOBOOTH_NO_CAMERA を付けるとカメラを初期化しない。
// そのとき begin() は成功を返し、grab() は常に nullptr (UI はプレビュー枠だけ描く)。
// カメラ SCCB と内部 I2C の共有が原因の不具合を切り分けるためのもの。
#pragma once

#include <esp_camera.h>

#include <cstddef>
#include <cstdint>

namespace hal {

class Camera {
 public:
  // M5.In_I2C.release() の後に esp_camera_init()。M5StackChan.begin() の後に呼ぶこと。
  // 失敗しても再度呼べる (ERROR の「再試行」)。
  bool begin();

  // 初期化済みでフレームを取れる状態か。
  bool ready() const { return ready_; }

  // PHOTOBOOTH_NO_CAMERA でビルドされたか。
  static constexpr bool disabled() {
#ifdef PHOTOBOOTH_NO_CAMERA
    return true;
#else
    return false;
#endif
  }

  // 1 フレーム取得する。使い終わったら必ず release()。取れなければ nullptr。
  camera_fb_t* grab();
  void release(camera_fb_t* fb);

  // 最後の begin() 失敗時の esp_err_t。
  int lastError() const { return last_error_; }

 private:
  bool ready_ = false;
  int last_error_ = 0;
};

// 候補フレームのコピー。PSRAM に確保し、clear() で解放する。
class FrameCopy {
 public:
  FrameCopy() = default;
  ~FrameCopy() { clear(); }
  FrameCopy(const FrameCopy&) = delete;
  FrameCopy& operator=(const FrameCopy&) = delete;

  // fb の内容をコピーする。同じサイズの領域が既にあれば使い回す。
  bool assign(const camera_fb_t& fb, uint32_t frame_id);
  void clear();

  bool valid() const { return buf_ != nullptr && len_ > 0; }
  const uint16_t* pixels() const { return reinterpret_cast<const uint16_t*>(buf_); }
  const uint8_t* data() const { return buf_; }
  size_t length() const { return len_; }
  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  uint32_t frameId() const { return frame_id_; }

 private:
  uint8_t* buf_ = nullptr;
  size_t capacity_ = 0;
  size_t len_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint32_t frame_id_ = 0;
};

}  // namespace hal
