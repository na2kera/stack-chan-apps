/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <esp_heap_caps.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include <cstdio>
#include <cstring>

#include "../hw/input.h"
#include "strings.h"
#include "widgets.h"

using namespace smooth_ui_toolkit::lvgl_cpp;

namespace photobooth::view {

namespace {

constexpr const char* kTag = "PB-View";

// プレビューに重ねる領域 (独立ファーム版と同じ)
// COMPOSE: 下端の案内帯
constexpr Rect kComposeBand{0, kScreenH - 40, kScreenW, 40};
// CAPTURE: 右上の人数・残り秒数
constexpr Rect kCaptureBox{kScreenW - 124, 0, 124, 84};

constexpr Rect kBody{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - kButtonBarH - 8};
constexpr Rect kBodyNoButtons{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - 8};

}  // namespace

View::View(hw::Input& input) : input_(input)
{
}

View::~View()
{
    end();
}

bool View::begin()
{
    if (preview_buf_ == nullptr) {
        preview_capacity_ = static_cast<size_t>(kScreenW) * kScreenH;
        preview_buf_      = static_cast<uint16_t*>(
            heap_caps_calloc(preview_capacity_, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (preview_buf_ == nullptr) {
            mclog::tagError(kTag, "failed to allocate preview buffer");
            preview_capacity_ = 0;
            return false;
        }
    }
    setPreviewSize(kScreenW, kScreenH);
    return true;
}

void View::end()
{
    {
        LvglLockGuard lock;
        preview_img_  = nullptr;
        label_faces_  = nullptr;
        label_remain_ = nullptr;
        page_.reset();
    }
    if (preview_buf_ != nullptr) {
        heap_caps_free(preview_buf_);
        preview_buf_      = nullptr;
        preview_capacity_ = 0;
    }
}

bool View::setPreviewSize(int width, int height)
{
    if (preview_buf_ == nullptr || width <= 0 || height <= 0 ||
        static_cast<size_t>(width) * height > preview_capacity_) {
        return false;
    }
    preview_dsc_                 = lv_image_dsc_t{};
    preview_dsc_.header.magic    = LV_IMAGE_HEADER_MAGIC;
    preview_dsc_.header.cf       = LV_COLOR_FORMAT_RGB565;
    preview_dsc_.header.w        = static_cast<uint32_t>(width);
    preview_dsc_.header.h        = static_cast<uint32_t>(height);
    preview_dsc_.header.stride   = static_cast<uint32_t>(width) * 2;
    preview_dsc_.data_size       = static_cast<uint32_t>(width) * height * 2;
    preview_dsc_.data            = reinterpret_cast<const uint8_t*>(preview_buf_);
    return true;
}

// LvglLockGuard の中で呼ぶ。
void View::newPage()
{
    preview_img_  = nullptr;
    label_faces_  = nullptr;
    label_remain_ = nullptr;
    page_.reset();
    page_ = std::make_unique<Page>(lv_screen_active());
    ++screen_id_;
}

// LvglLockGuard の中で呼ぶ。
void View::addButtons(const char* const* labels, int count)
{
    const uint32_t id = screen_id_;
    buttonBar(*page_, labels, count, [this, id](int index) { input_.pushButton(id, index); });
}

// LvglLockGuard の中で呼ぶ。全面のプレビュー (またはカメラ無効の枠)。
void View::addPreview(bool camera_ok)
{
    if (!camera_ok || preview_buf_ == nullptr) {
        auto& frame = panel(*page_, page_->root(), Rect{0, 0, kScreenW, kScreenH}, lv_color_make(128, 128, 128));
        frame.setBorderWidth(1);
        frame.setBorderColor(lv_color_hex(0xFFFFFF));
        textBox(*page_, frame.get(), Rect{0, 0, kScreenW, kScreenH}, str::kCameraDisabled, font::body(),
                lv_color_hex(0xFFFFFF), Align::Center);
        return;
    }
    // 前の画面 (REVIEW の候補など) が一瞬見えないよう、最初のフレームが来るまでは黒にしておく。
    std::memset(preview_buf_, 0, preview_capacity_ * sizeof(uint16_t));
    auto& img = page_->add<Image>(page_->root());
    img.setSrc(&preview_dsc_);
    img.align(LV_ALIGN_CENTER, 0, 0);
    preview_img_ = img.get();
}

void View::showIdle(bool pc_online, const char* warning)
{
    LvglLockGuard lock;
    newPage();
    const uint32_t id = screen_id_;
    // ボタン以外のどこを触っても開始 (root は lv_obj なので既定でクリックを受ける)。
    page_->rootObj().onClick().connect([this, id]() { input_.pushScreenTap(id); });

    titleBar(*page_, str::kTitleIdle);
    const Rect prompt{8, kTitleH + 24, kScreenW - 16, 56};
    textBox(*page_, page_->root(), prompt, str::kIdlePrompt, font::body(), color::text(), Align::Center);
    const Rect touch{8, prompt.y + prompt.h + 4, kScreenW - 16, 28};
    textBox(*page_, page_->root(), touch, str::kIdleTouchStart, font::body(), color::muted(), Align::Center);

    // ボタン帯のすぐ上の 1 行: 左に警告、右に PC 接続状態 (独立ファーム版は画面下端に置いていた)。
    const int32_t status_y = kScreenH - kButtonBarH - 26;
    if (warning != nullptr) {
        textBox(*page_, page_->root(), Rect{8, status_y, 200, 24}, warning, font::body(), color::warn(), Align::Left);
    }
    if (!pc_online) {
        auto& pc = textBox(*page_, page_->root(), Rect{kScreenW - 108, status_y, 100, 24}, str::kPcOffline,
                           font::body(), color::muted(), Align::Left);
        pc.setTextAlign(LV_TEXT_ALIGN_RIGHT);
    }

    static const char* const kLabels[] = {str::kBtnExit};
    addButtons(kLabels, 1);
}

void View::showAnnounce(const char* title)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBodyNoButtons, str::kAnnounce, font::body(), color::text(), Align::Center);
}

void View::showCompose(bool camera_ok)
{
    LvglLockGuard lock;
    newPage();
    addPreview(camera_ok);
    auto& band = panel(*page_, page_->root(), kComposeBand, color::overlayBg());
    textBox(*page_, band.get(), Rect{0, 0, kComposeBand.w, kComposeBand.h}, str::kCompose, font::body(),
            color::overlayText(), Align::Center);
}

void View::showCapture(bool camera_ok)
{
    LvglLockGuard lock;
    newPage();
    addPreview(camera_ok);
    auto& box = panel(*page_, page_->root(), kCaptureBox, color::overlayBg());

    auto& faces = page_->add<Label>(box.get());
    faces.setTextFont(font::body());
    faces.setTextColor(color::overlayText());
    faces.setText("");
    faces.align(LV_ALIGN_TOP_MID, 0, 4);
    label_faces_ = faces.get();

    auto& remain = page_->add<Label>(box.get());
    remain.setTextFont(font::large());
    remain.setTextColor(color::overlayText());
    remain.setText("");
    remain.align(LV_ALIGN_TOP_MID, 0, 30);
    label_remain_ = remain.get();
}

void View::updateCaptureOverlay(int remaining_sec, int face_count)
{
    LvglLockGuard lock;
    if (label_faces_ == nullptr || label_remain_ == nullptr) {
        return;
    }
    char buf[24];
    if (face_count < 0) {
        snprintf(buf, sizeof(buf), "%s %s", str::kFaceCountLabel, str::kFaceCountNone);
    } else {
        snprintf(buf, sizeof(buf), "%s %d", str::kFaceCountLabel, face_count);
    }
    lv_label_set_text(label_faces_, buf);
    lv_obj_align(label_faces_, LV_ALIGN_TOP_MID, 0, 4);
    snprintf(buf, sizeof(buf), "%d", remaining_sec);
    lv_label_set_text(label_remain_, buf);
    lv_obj_align(label_remain_, LV_ALIGN_TOP_MID, 0, 30);
}

void View::updatePreview(const uint16_t* pixels, int width, int height)
{
    if (pixels == nullptr) {
        return;
    }
    LvglLockGuard lock;
    if (preview_img_ == nullptr) {
        return;
    }
    const bool resized = static_cast<int>(preview_dsc_.header.w) != width ||
                         static_cast<int>(preview_dsc_.header.h) != height;
    if (resized && !setPreviewSize(width, height)) {
        mclog::tagWarn(kTag, "preview size {}x{} not supported", width, height);
        return;
    }
    // LVGL タスクが描画中に書き換えないよう、ロックの中でコピーしてから無効化する。
    std::memcpy(preview_buf_, pixels, static_cast<size_t>(width) * height * 2);
    if (resized) {
        lv_image_set_src(preview_img_, &preview_dsc_);
    }
    // 純正は画像キャッシュ無効 (CONFIG_LV_CACHE_DEF_SIZE=0) なので、無効化だけで新しい中身が描かれる。
    lv_obj_invalidate(preview_img_);
}

bool View::showReview(const char* title, const uint16_t* pixels, int width, int height)
{
    LvglLockGuard lock;
    newPage();
    if (pixels != nullptr && setPreviewSize(width, height)) {
        std::memcpy(preview_buf_, pixels, static_cast<size_t>(width) * height * 2);
        auto& img = page_->add<Image>(page_->root());
        img.setSrc(&preview_dsc_);
        img.align(LV_ALIGN_CENTER, 0, 0);
        titleBar(*page_, title);
        static const char* const kLabels[] = {str::kBtnSave, str::kBtnRetake};
        addButtons(kLabels, 2);
        return true;
    }
    if (pixels != nullptr) {
        mclog::tagWarn(kTag, "review candidate {}x{} cannot be shown (no preview buffer)", width, height);
    }
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBody, str::kNoCandidate, font::body(), color::text(), Align::Center);
    static const char* const kLabels[] = {str::kBtnRetake};
    addButtons(kLabels, 1);
    return false;
}

void View::showUploading(const char* title)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBodyNoButtons, str::kUploading, font::body(), color::text(), Align::Center);
}

void View::showPhotoQr(const char* title, const char* photo_url, const char* expires_at)
{
    LvglLockGuard lock;
    newPage();
    qr(*page_, kQrX, kQrY, kQrSize, photo_url);
    // 右欄: 1 行目に状態名、その下に削除予定時刻
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY, kQrTextW, 28}, title, font::body(), color::text(),
            Align::Left);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY + 44, kQrTextW, 24}, str::kExpiresLabel, font::body(),
            color::text(), Align::Left);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY + 68, kQrTextW, 28}, expires_at, font::body(), color::text(),
            Align::Left);
    static const char* const kLabels[] = {str::kBtnNext, str::kBtnRetake};
    addButtons(kLabels, 2);
}

void View::showXQr(const char* title, const char* share_url)
{
    LvglLockGuard lock;
    newPage();
    qr(*page_, kQrX, kQrY, kQrSize, share_url);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY, kQrTextW, 28}, title, font::body(), color::text(),
            Align::Left);
    const int32_t y = kQrY + 40;
    textBox(*page_, page_->root(), Rect{kQrTextX, y, kQrTextW, kScreenH - kButtonBarH - 4 - y}, str::kXQrGuide,
            font::body(), color::text(), Align::Left);
    static const char* const kLabels[] = {str::kBtnBack, str::kBtnExit};
    addButtons(kLabels, 2);
}

void View::showError(const char* title, const char* reason)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBody, reason, font::body(), color::text(), Align::Center);
    static const char* const kLabels[] = {str::kBtnRetry, str::kBtnExit};
    addButtons(kLabels, 2);
}

}  // namespace photobooth::view
