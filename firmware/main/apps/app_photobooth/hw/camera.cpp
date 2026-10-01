/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "camera.h"

#include <esp_heap_caps.h>
#include <esp_imgfx_color_convert.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/board/hal_bridge.h>
#include <hal/hal.h>
#include <linux/videodev2.h>
#include <mooncake_log.h>

#include <cstring>
#include <utility>

#include "../config.h"

namespace photobooth::hw {

namespace {

constexpr const char* kTag = "PB-Camera";
// 純正 StackChanCamera のログタグ。StreamCaptures() が毎フレーム WARN を出すので、取り込み中は抑える。
constexpr const char* kStockCameraTag = "StackChanCamera";
constexpr uint32_t kTaskStack   = 6144;
constexpr UBaseType_t kTaskPrio = 3;
// LVGL タスク (stackchan_display.cc: 優先度 3, core 1) と取り合わないよう core 0 に置く。
constexpr BaseType_t kTaskCore = 0;

StackChanCamera* cam(void* p)
{
    return static_cast<StackChanCamera*>(p);
}

void fourcc(int f, char out[5])
{
    out[0] = static_cast<char>(f & 0xff);
    out[1] = static_cast<char>((f >> 8) & 0xff);
    out[2] = static_cast<char>((f >> 16) & 0xff);
    out[3] = static_cast<char>((f >> 24) & 0xff);
    out[4] = '\0';
}

}  // namespace

// 取り込みタスクと Camera が共有する状態。両方が shared_ptr で持ち、最後に手放した側が解放する。
// end() がタスクを待ちきれずに切り離しても、タスクが終わるときにここで後始末される。
struct CameraWorker {
    StackChanCamera* camera = nullptr;
    std::atomic<bool> streaming{false};
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};
    std::atomic<uint32_t> seq{0};

    std::mutex mutex;  // latest とその寸法を保護
    uint16_t* back    = nullptr;  // 取り込みタスクが書く
    uint16_t* latest  = nullptr;  // 最新の完成フレーム
    size_t buf_pixels = 0;
    uint16_t latest_w = 0;
    uint16_t latest_h = 0;
    uint32_t latest_captured_ms = 0;

    // esp_imgfx の変換ハンドル (入力形式・寸法が変わったら作り直す)。タスクだけが触る
    esp_imgfx_color_convert_handle_t converter = nullptr;
    int conv_format    = 0;
    int conv_w         = 0;
    int conv_h         = 0;
    bool format_logged = false;

    ~CameraWorker()
    {
        closeConverter();
        freeBuffers();
    }

    void run();
    bool ensureBuffers(size_t pixels);
    void freeBuffers();
    bool convert(const uint8_t* src, size_t len, int w, int h, int format, uint16_t* dst);
    void closeConverter();
};

namespace {

// 切り離したタスクの状態。Camera はアプリを開くたびに作り直されるので、ファイルスコープで覚える。
std::weak_ptr<CameraWorker> g_detached;

void taskEntry(void* arg)
{
    // 引数はタスク用の shared_ptr のコピー (new で渡したもの)
    auto* holder = static_cast<std::shared_ptr<CameraWorker>*>(arg);
    (*holder)->run();
    (*holder)->running = false;
    delete holder;  // 切り離されていれば、ここで CameraWorker が解放される
    vTaskDelete(nullptr);
}

}  // namespace

bool Camera::busy()
{
    auto w = g_detached.lock();
    return w != nullptr && w->running.load();
}

bool Camera::begin()
{
    if (busy()) {
        mclog::tagError(kTag, "previous camera task is still running; cannot start");
        return false;
    }
    auto* camera = hal_bridge::board_get_camera();
    if (camera == nullptr) {
        mclog::tagError(kTag, "board camera not available");
        return false;
    }
    camera_ = camera;

    if (config::CAMERA_HMIRROR) {
        camera->SetHMirror(true);
    }
    if (config::CAMERA_VFLIP) {
        camera->SetVFlip(true);
    }

    saved_log_level_ = static_cast<int>(esp_log_level_get(kStockCameraTag));
    esp_log_level_set(kStockCameraTag, ESP_LOG_ERROR);

    worker_          = std::make_shared<CameraWorker>();
    worker_->camera  = camera;
    worker_->running = true;
    auto* holder     = new std::shared_ptr<CameraWorker>(worker_);
    if (xTaskCreatePinnedToCore(taskEntry, "pb_camera", kTaskStack, holder, kTaskPrio, nullptr, kTaskCore) !=
        pdPASS) {
        mclog::tagError(kTag, "failed to create camera task");
        delete holder;
        worker_.reset();
        restoreStockSettings();
        return false;
    }
    mclog::tagInfo(kTag, "begin: {}x{}", camera->GetFrameWidth(), camera->GetFrameHeight());
    return true;
}

void Camera::restoreStockSettings()
{
    if (camera_ != nullptr) {
        if (config::CAMERA_HMIRROR) {
            cam(camera_)->SetHMirror(false);  // 純正の既定 (stackchan.cc の InitializeCamera)
        }
        if (config::CAMERA_VFLIP) {
            cam(camera_)->SetVFlip(false);
        }
    }
    if (saved_log_level_ >= 0) {
        esp_log_level_set(kStockCameraTag, static_cast<esp_log_level_t>(saved_log_level_));
        saved_log_level_ = -1;
    }
    camera_ = nullptr;
}

void Camera::end()
{
    if (!worker_) {
        restoreStockSettings();
        return;
    }
    worker_->quit      = true;
    worker_->streaming = false;
    // StreamCaptures() は最大 1 フレーム分 (20 fps で 50 ms) 待つので、少し長めに待つ。
    for (int i = 0; i < 100 && worker_->running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (worker_->running.load()) {
        // タスクが StreamCaptures() (VIDIOC_DQBUF) から戻ってこない。待ち続けるとアプリが閉じられないので
        // 切り離す。共有状態 (バッファ・変換ハンドル) はタスクが終わるときに解放される。
        mclog::tagWarn(kTag, "camera task did not stop in 1 s; detached (restart is refused until it exits)");
        g_detached = worker_;
    } else {
        mclog::tagInfo(kTag, "end");
    }
    worker_.reset();
    restoreStockSettings();
}

bool Camera::restart()
{
    mclog::tagInfo(kTag, "restart");
    end();
    return begin();
}

bool Camera::ready() const
{
    return worker_ != nullptr && worker_->running.load();
}

uint32_t Camera::latestSeq() const
{
    return worker_ ? worker_->seq.load() : 0;
}

void Camera::setStreaming(bool on)
{
    if (!worker_) {
        return;
    }
    if (worker_->streaming.load() != on) {
        mclog::tagInfo(kTag, "streaming {}", on ? "on" : "off");
    }
    worker_->streaming = on;
}

bool Camera::lockLatest(uint32_t last_seq, FrameView& out)
{
    if (!worker_ || worker_->seq.load() == last_seq) {
        return false;
    }
    worker_->mutex.lock();
    if (worker_->latest == nullptr || worker_->latest_w == 0) {
        worker_->mutex.unlock();
        return false;
    }
    out.pixels = worker_->latest;
    out.width  = worker_->latest_w;
    out.height = worker_->latest_h;
    out.seq         = worker_->seq.load();
    out.captured_ms = worker_->latest_captured_ms;
    return true;
}

void Camera::unlock()
{
    worker_->mutex.unlock();
}

// ---- 取り込みタスク ----------------------------------------------------------

void CameraWorker::run()
{
    uint32_t failures = 0;
    while (!quit.load()) {
        if (!streaming.load()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!camera->StreamCaptures()) {
            if (++failures % 20 == 1) {
                mclog::tagWarn(kTag, "StreamCaptures failed ({} times)", failures);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        failures = 0;
        // 首が動いていた間のフレームを候補から外すため、取得した時刻を覚えておく (spec §4)。
        const uint32_t captured_ms = GetHAL().millis();

        const uint8_t* data = camera->GetFrameData();
        const size_t len    = camera->GetFrameSize();
        const int w         = camera->GetFrameWidth();
        const int h         = camera->GetFrameHeight();
        const int format    = camera->GetFrameFormat();
        if (!format_logged) {
            // 形式の確認用 (画像のバイト列はログに出さない)
            char cc[5];
            fourcc(format, cc);
            mclog::tagInfo(kTag, "first frame: format '{}' (0x{:08x}) {}x{} {} bytes", cc, format, w, h, len);
            format_logged = true;
        }
        if (data == nullptr || w <= 0 || h <= 0 || !ensureBuffers(static_cast<size_t>(w) * h)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!convert(data, len, w, h, format, back)) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            std::swap(back, latest);
            latest_w = static_cast<uint16_t>(w);
            latest_h = static_cast<uint16_t>(h);
            latest_captured_ms = captured_ms;
            seq.fetch_add(1);
        }
        vTaskDelay(1);  // LVGL / メインループに CPU を譲る
    }
}

bool CameraWorker::ensureBuffers(size_t pixels)
{
    if (back != nullptr && latest != nullptr && buf_pixels == pixels) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (back != nullptr) {
        heap_caps_free(back);
    }
    if (latest != nullptr) {
        heap_caps_free(latest);
    }
    back       = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    latest     = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    latest_w   = 0;
    latest_h   = 0;
    buf_pixels = pixels;
    if (back == nullptr || latest == nullptr) {
        mclog::tagError(kTag, "failed to allocate frame buffers ({} bytes x2)", pixels * 2);
        if (back != nullptr) heap_caps_free(back);
        if (latest != nullptr) heap_caps_free(latest);
        back       = nullptr;
        latest     = nullptr;
        buf_pixels = 0;
        return false;
    }
    return true;
}

void CameraWorker::freeBuffers()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (back != nullptr) heap_caps_free(back);
    if (latest != nullptr) heap_caps_free(latest);
    back       = nullptr;
    latest     = nullptr;
    buf_pixels = 0;
    latest_w   = 0;
    latest_h   = 0;
}

bool CameraWorker::convert(const uint8_t* src, size_t len, int w, int h, int format, uint16_t* dst)
{
    const size_t out_len = static_cast<size_t>(w) * h * 2;
    if (format == V4L2_PIX_FMT_RGB565) {
        // 純正は RGB565 を LE にそろえて渡す (stackchan_camera.cc)。そのままコピーする。
        if (len < out_len) {
            mclog::tagWarn(kTag, "short RGB565 frame: {} < {}", len, out_len);
            return false;
        }
        std::memcpy(dst, src, out_len);
        return true;
    }

    // YUYV など: 純正のプレビュー (StackChanCamera::Capture) と同じく esp_imgfx で RGB565 LE へ。
    if (converter == nullptr || conv_format != format || conv_w != w || conv_h != h) {
        closeConverter();
        esp_imgfx_color_convert_cfg_t cfg = {
            .in_res          = {.width = static_cast<int16_t>(w), .height = static_cast<int16_t>(h)},
            .in_pixel_fmt    = static_cast<esp_imgfx_pixel_fmt_t>(format),
            .out_pixel_fmt   = ESP_IMGFX_PIXEL_FMT_RGB565_LE,
            .color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601,
        };
        esp_imgfx_color_convert_handle_t handle = nullptr;
        if (esp_imgfx_color_convert_open(&cfg, &handle) != ESP_IMGFX_ERR_OK || handle == nullptr) {
            char cc[5];
            fourcc(format, cc);
            mclog::tagError(kTag, "no converter for format '{}' {}x{}", cc, w, h);
            return false;
        }
        converter   = handle;
        conv_format = format;
        conv_w      = w;
        conv_h      = h;
    }
    esp_imgfx_data_t in  = {.data = const_cast<uint8_t*>(src), .data_len = static_cast<uint32_t>(len)};
    esp_imgfx_data_t out = {.data = reinterpret_cast<uint8_t*>(dst), .data_len = static_cast<uint32_t>(out_len)};
    if (esp_imgfx_color_convert_process(converter, &in, &out) != ESP_IMGFX_ERR_OK) {
        mclog::tagWarn(kTag, "color convert failed");
        return false;
    }
    return true;
}

void CameraWorker::closeConverter()
{
    if (converter != nullptr) {
        esp_imgfx_color_convert_close(converter);
        converter = nullptr;
    }
}

// ---- FrameCopy -------------------------------------------------------------

bool FrameCopy::assign(const FrameView& frame, uint32_t frame_id)
{
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    if (frame.pixels == nullptr || pixels == 0) {
        return false;
    }
    if (buf_ == nullptr || capacity_ != pixels) {
        clear();
        buf_ = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf_ == nullptr) {
            mclog::tagError(kTag, "failed to allocate candidate ({} bytes)", pixels * 2);
            return false;
        }
        capacity_ = pixels;
    }
    std::memcpy(buf_, frame.pixels, pixels * 2);
    width_    = frame.width;
    height_   = frame.height;
    frame_id_ = frame_id;
    return true;
}

void FrameCopy::clear()
{
    if (buf_ != nullptr) {
        heap_caps_free(buf_);
    }
    buf_      = nullptr;
    capacity_ = 0;
    width_    = 0;
    height_   = 0;
    frame_id_ = 0;
}

}  // namespace photobooth::hw
