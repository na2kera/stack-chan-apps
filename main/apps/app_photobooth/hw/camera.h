/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// カメラのラッパ (docs/design/step1-device.md §5 camera を純正の StackChanCamera に移植)。
//
// 純正のビデオ通話 (main/hal/hal_ws_avatar.cpp) と同じく hal_bridge::board_get_camera() の
// StreamCaptures() / GetFrameData() ... を使う。カメラ本体は純正のボード初期化が作って持っているので、
// ここでは作り直さない (deinit / init はしない)。
//
// StreamCaptures() はフレームが来るまで待つので、専用タスクで回して RGB565 (LE、LVGL の
// LV_COLOR_FORMAT_RGB565 と同じ並び) に変換し、最新 1 枚をダブルバッファで渡す。
// 純正の GC0308 設定 (sdkconfig: DVP YUV422 320x240) では YUYV が来るので esp_imgfx で変換する。
#pragma once

#include <atomic>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace photobooth::hw {

// 変換済みの 1 フレーム (RGB565 LE)。Camera::lockLatest() 〜 unlock() の間だけ有効。
struct FrameView {
    const uint16_t* pixels = nullptr;
    uint16_t width         = 0;
    uint16_t height        = 0;
    uint32_t seq           = 0;  // 変換したフレームの通し番号 (1 から)
    uint32_t captured_ms   = 0;  // StreamCaptures() から戻った時刻 (GetHAL().millis())
};

struct CameraWorker;

class Camera {
public:
    // カメラの有無を確かめ、取り込みタスクを作る (まだ取り込まない)。カメラが無い、
    // または前回止めきれなかったタスクがまだ生きている (busy()) ときは false。
    bool begin();
    // 取り込みタスクを止める。向きとログレベルを純正の既定に戻す。
    // タスクが 1 秒以内に止まらなければ切り離す (共有状態はタスクが終わるときに自分で解放する)。
    void end();
    // end() → begin()。ERROR の「再試行」用 (純正のカメラドライバ自体は作り直さない)。
    // 前のタスクが切り離されたまま生きていれば false (busy())。
    bool restart();

    bool ready() const;

    // 切り離した取り込みタスクがまだ終わっていないか (Camera の作り直しをまたいで覚えている)。
    static bool busy();

    // 取り込みの開始・停止 (COMPOSE / CAPTURE の間だけ回す)。
    void setStreaming(bool on);

    // last_seq より新しいフレームがあれば、取り込みタスクとの共有を止めて (mutex を取って) true を返す。
    // true のときは使い終わったら必ず unlock() する。false のときは何もしなくてよい。
    bool lockLatest(uint32_t last_seq, FrameView& out);
    void unlock();

    // 直近の変換済みフレーム番号 (取り込み開始時の基準に使う)。
    uint32_t latestSeq() const;

private:
    void restoreStockSettings();

    // タスクと共有する状態。タスクも shared_ptr を持つので、Camera が先に消えても安全。
    std::shared_ptr<CameraWorker> worker_;
    void* camera_        = nullptr;  // StackChanCamera* (純正ボードが持つ。解放しない)
    int saved_log_level_ = -1;
};

// 候補フレームのコピー。PSRAM に確保し、clear() で解放する。
class FrameCopy {
public:
    FrameCopy() = default;
    ~FrameCopy()
    {
        clear();
    }
    FrameCopy(const FrameCopy&)            = delete;
    FrameCopy& operator=(const FrameCopy&) = delete;

    // frame の内容をコピーする。同じ大きさの領域が既にあれば使い回す。
    bool assign(const FrameView& frame, uint32_t frame_id);
    void clear();

    bool valid() const
    {
        return buf_ != nullptr && width_ > 0 && height_ > 0;
    }
    const uint16_t* pixels() const
    {
        return buf_;
    }
    size_t length() const
    {
        return static_cast<size_t>(width_) * height_ * 2;
    }
    uint16_t width() const
    {
        return width_;
    }
    uint16_t height() const
    {
        return height_;
    }
    uint32_t frameId() const
    {
        return frame_id_;
    }

private:
    uint16_t* buf_     = nullptr;
    size_t capacity_   = 0;  // 画素数
    uint16_t width_    = 0;
    uint16_t height_   = 0;
    uint32_t frame_id_ = 0;
};

}  // namespace photobooth::hw
