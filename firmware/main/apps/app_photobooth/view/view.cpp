/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "view.h"

#include <esp_heap_caps.h>
#include <hal/hal.h>
#include <hal/utils/jpeg_to_image/jpeg_decoder.h>
#include <mooncake_log.h>

#include <cstdio>
#include <cstring>

#include "../hw/input.h"
#include "jpeg_info.h"
#include "strings.h"
#include "widgets.h"

using namespace smooth_ui_toolkit::lvgl_cpp;

namespace photobooth::view {

namespace {

constexpr const char* kTag = "PB-View";

// プレビューに重ねる領域 (独立ファーム版と同じ)
// COMPOSE / CAPTURE: 下端の案内帯
constexpr Rect kBand{0, kScreenH - 40, kScreenW, 40};
// CAPTURE: 右上の人数・残り秒数
// プレビューを隠しすぎないよう小さめにする (124x84 → 88x56)。上の行に人数、下に残り秒数。
constexpr Rect kCaptureBox{kScreenW - 88, 0, 88, 56};

constexpr Rect kBody{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - kButtonBarH - 8};
constexpr Rect kBodyNoButtons{8, kTitleH + 4, kScreenW - 16, kScreenH - kTitleH - 8};

// 待機: ボタン帯のすぐ上の 1 行。左に警告、右に接続状態。
constexpr int32_t kIdleStatusY = kScreenH - kButtonBarH - 26;
constexpr int32_t kIdleLinkW   = 100;
constexpr Rect kIdleLink{kScreenW - 8 - kIdleLinkW, kIdleStatusY, kIdleLinkW, 24};
constexpr Rect kIdleWarning{8, kIdleStatusY, kScreenW - 16 - kIdleLinkW - 4, 24};

const char* idleLinkText(IdleLink link)
{
    return link == IdleLink::Online ? str::kPcOnline : str::kPcOffline;
}

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
        label_status_ = nullptr;
        band_panel_   = nullptr;
        band_label_   = nullptr;
        label_diag_   = nullptr;
        label_wifi_   = nullptr;
        page_.reset();
        review_image_.reset();  // lv_image を消してから画像を解放する
        captured_image_.reset();
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
    label_status_ = nullptr;
    band_panel_   = nullptr;
    band_label_   = nullptr;
    label_diag_   = nullptr;
    label_wifi_   = nullptr;
    page_.reset();
    review_image_.reset();  // 前の画面の lv_image を消してから画像を解放する
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
    preview_has_frame_ = false;
    auto& img = page_->add<Image>(page_->root());
    img.setSrc(&preview_dsc_);
    img.align(LV_ALIGN_CENTER, 0, 0);
    preview_img_ = img.get();
}

// LvglLockGuard の中で呼ぶ。下端の案内帯 (プレビューに重ねる)。
void View::addBand(const char* text, bool hidden)
{
    auto& band = panel(*page_, page_->root(), kBand, color::overlayBg());
    auto& label = textBox(*page_, band.get(), Rect{0, 0, kBand.w, kBand.h}, text, font::body(), color::overlayText(),
                          Align::Center);
    band_panel_ = band.get();
    band_label_ = label.get();
    if (hidden) {
        lv_obj_add_flag(band_panel_, LV_OBJ_FLAG_HIDDEN);
    }
}

void View::showWifiConnecting()
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, str::kWifiConnecting);
    auto& label = textBox(*page_, page_->root(), kBodyNoButtons, "", font::body(), color::text(), Align::Center);
    label_wifi_ = label.get();
}

void View::setWifiMessage(std::string_view text)
{
    // 純正の文言 (英語。"WiFi scanning..." など) をそのまま出す。App Center の読み込み画面と同じ。
    char buf[160];
    snprintf(buf, sizeof(buf), "%.*s", static_cast<int>(text.size()), text.data());
    LvglLockGuard lock;
    if (label_wifi_ != nullptr) {
        lv_label_set_text(label_wifi_, buf);
    }
}

void View::showIdle(IdleLink link, const char* warning)
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
    if (warning != nullptr) {
        textBox(*page_, page_->root(), kIdleWarning, warning, font::body(), color::warn(), Align::Left);
    }
    auto& pc = textBox(*page_, page_->root(), kIdleLink, idleLinkText(link), font::body(), color::muted(), Align::Left);
    pc.setTextAlign(LV_TEXT_ALIGN_RIGHT);
    label_status_ = pc.get();

    static const char* const kLabels[] = {str::kBtnExit};
    addButtons(kLabels, 1);
}

void View::updateIdleStatus(IdleLink link)
{
    LvglLockGuard lock;
    if (label_status_ != nullptr) {
        lv_label_set_text(label_status_, idleLinkText(link));
    }
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
    addBand(str::kCompose, false);
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
    faces.align(LV_ALIGN_TOP_MID, 0, 2);
    label_faces_ = faces.get();

    auto& remain = page_->add<Label>(box.get());
    remain.setTextFont(font::large());
    remain.setTextColor(color::overlayText());
    remain.setText("");
    remain.align(LV_ALIGN_TOP_MID, 0, 24);
    label_remain_ = remain.get();

    // 案内帯は hint があるときだけ出す (setBand)。
    addBand(str::kCompose, true);
}

void View::setBand(const char* text)
{
    LvglLockGuard lock;
    if (band_panel_ == nullptr || band_label_ == nullptr) {
        return;
    }
    if (text == nullptr) {
        lv_obj_add_flag(band_panel_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(band_label_, text);
    lv_obj_remove_flag(band_panel_, LV_OBJ_FLAG_HIDDEN);
}

void View::updateCaptureOverlay(int remaining_sec, int face_count, int target)
{
    LvglLockGuard lock;
    if (label_faces_ == nullptr || label_remain_ == nullptr) {
        return;
    }
    char buf[32];
    if (face_count < 0) {
        snprintf(buf, sizeof(buf), "%s %s", str::kFaceCountLabel, str::kFaceCountNone);
    } else if (target <= 0) {
        snprintf(buf, sizeof(buf), "%s %d/%s", str::kFaceCountLabel, face_count, str::kFaceCountNone);
    } else {
        snprintf(buf, sizeof(buf), "%s %d/%d", str::kFaceCountLabel, face_count, target);
    }
    lv_label_set_text(label_faces_, buf);
    lv_obj_align(label_faces_, LV_ALIGN_TOP_MID, 0, 2);
    snprintf(buf, sizeof(buf), "%d", remaining_sec);
    lv_label_set_text(label_remain_, buf);
    lv_obj_align(label_remain_, LV_ALIGN_TOP_MID, 0, 24);
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
    preview_has_frame_ = true;
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
        preview_has_frame_ = true;
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

std::shared_ptr<LvglAllocatedImage> View::decodeCandidateJpeg(const uint8_t* jpeg, size_t len)
{
    // デコード用の領域を確保する前に、ヘッダだけ読んで大きさを確かめる。プレビューと同じ大きさ
    // (320x240) 以外は出さない (edge は受信フレームと同じ大きさの JPEG を返す、protocol.md)。
    JpegInfo info;
    const JpegStatus st = readJpegInfo(jpeg, len, info);
    if (st == JpegStatus::Invalid) {
        mclog::tagWarn(kTag, "candidate jpeg ({} bytes) has no valid header", len);
        return nullptr;
    }
    if (st == JpegStatus::Unsupported) {
        mclog::tagWarn(kTag, "candidate jpeg {}x{} is not baseline (SOF 0x{:02X}, {} bit, {} components)", info.width,
                       info.height, static_cast<unsigned>(info.sof), info.precision, info.components);
        return nullptr;
    }
    if (info.width != kScreenW || info.height != kScreenH) {
        mclog::tagWarn(kTag, "candidate jpeg {}x{} is not the expected {}x{}", info.width, info.height, kScreenW,
                       kScreenH);
        return nullptr;
    }
    // デコード (数十 ms) は LVGL のロックの外でする。出力は RGB565 LE (プレビューと同じ並び)。
    auto image = jpeg_dec::decode_to_lvgl(jpeg, len);
    if (image == nullptr || image->image_dsc() == nullptr) {
        mclog::tagWarn(kTag, "candidate jpeg ({} bytes) cannot be decoded", len);
        return nullptr;
    }
    const auto* dsc = image->image_dsc();
    if (static_cast<int32_t>(dsc->header.w) != kScreenW || static_cast<int32_t>(dsc->header.h) != kScreenH) {
        mclog::tagWarn(kTag, "candidate jpeg decoded to {}x{}", static_cast<int>(dsc->header.w),
                       static_cast<int>(dsc->header.h));
        return nullptr;
    }
    return image;
}

bool View::showReviewJpeg(const char* title, const uint8_t* jpeg, size_t len)
{
    auto image = decodeCandidateJpeg(jpeg, len);
    if (image == nullptr) {
        return false;
    }
    LvglLockGuard lock;
    newPage();
    review_image_ = std::move(image);  // 画面が参照している間は持ち続ける (newPage / end で解放)
    auto& img     = page_->add<Image>(page_->root());
    img.setSrc(review_image_->image_dsc());
    img.align(LV_ALIGN_CENTER, 0, 0);
    titleBar(*page_, title);
    static const char* const kLabels[] = {str::kBtnSave, str::kBtnRetake};
    addButtons(kLabels, 2);
    return true;
}

void View::showShutterFlash()
{
    LvglLockGuard lock;
    newPage();
    // 白は「フラッシュ」の意味なので、背景色 (color::bg) とは別に明示する。
    panel(*page_, page_->root(), Rect{0, 0, kScreenW, kScreenH}, lv_color_hex(0xFFFFFF));
}

bool View::prepareCapturedJpeg(const uint8_t* jpeg, size_t len)
{
    auto image = decodeCandidateJpeg(jpeg, len);
    if (image == nullptr) {
        return false;
    }
    LvglLockGuard lock;  // 前に用意したもの (表示していない) を差し替える
    captured_image_ = std::move(image);
    return true;
}

CapturedSource View::showCaptured(const char* title)
{
    LvglLockGuard lock;
    newPage();
    CapturedSource src = CapturedSource::None;
    if (captured_image_ != nullptr) {
        review_image_ = std::move(captured_image_);  // 以後は画面が持つ (newPage / end で解放)
        auto& img     = page_->add<Image>(page_->root());
        img.setSrc(review_image_->image_dsc());
        img.align(LV_ALIGN_CENTER, 0, 0);
        src = CapturedSource::Candidate;
    } else if (preview_buf_ != nullptr && preview_has_frame_) {
        // CAPTURE で最後に描いたフレームがバッファに残っている (取り込みは止めてあるので書き換わらない)。
        auto& img = page_->add<Image>(page_->root());
        img.setSrc(&preview_dsc_);
        img.align(LV_ALIGN_CENTER, 0, 0);
        src = CapturedSource::Preview;
    }
    titleBar(*page_, title);
    return src;
}

void View::discardCapturedJpeg()
{
    LvglLockGuard lock;
    captured_image_.reset();
}

void View::showReviewEmpty(const char* title, const char* text)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBody, text, font::body(), color::text(), Align::Center);
    static const char* const kLabels[] = {str::kBtnRetake};
    addButtons(kLabels, 1);
}

void View::showUploading(const char* title, bool captured)
{
    char text[64];
    if (captured) {
        snprintf(text, sizeof(text), "%s\n%s", str::kCaptured, str::kUploading);
    } else {
        snprintf(text, sizeof(text), "%s", str::kUploading);
    }
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBodyNoButtons, text, font::body(), color::text(), Align::Center);
}

bool View::showPhotoQr(const char* title, const char* photo_url, const char* expires_at)
{
    LvglLockGuard lock;
    newPage();
    const bool ok = qr(*page_, kQrX, kQrY, kQrSize, photo_url);
    // 右欄: 1 行目に状態名、その下に削除予定時刻
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY, kQrTextW, 28}, title, font::body(), color::text(),
            Align::Left);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY + 44, kQrTextW, 24}, str::kExpiresLabel, font::body(),
            color::text(), Align::Left);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY + 68, kQrTextW, 28}, expires_at, font::body(), color::text(),
            Align::Left);
    static const char* const kLabels[] = {str::kBtnNext, str::kBtnRetake};
    addButtons(kLabels, 2);
    return ok;
}

bool View::showXQr(const char* title, const char* share_url)
{
    LvglLockGuard lock;
    newPage();
    const bool ok = qr(*page_, kQrX, kQrY, kQrSize, share_url);
    textBox(*page_, page_->root(), Rect{kQrTextX, kQrY, kQrTextW, 28}, title, font::body(), color::text(),
            Align::Left);
    const int32_t y = kQrY + 40;
    textBox(*page_, page_->root(), Rect{kQrTextX, y, kQrTextW, kScreenH - kButtonBarH - 4 - y}, str::kXQrGuide,
            font::body(), color::text(), Align::Left);
    static const char* const kLabels[] = {str::kBtnBack, str::kBtnExit};
    addButtons(kLabels, 2);
    return ok;
}

void View::showError(const char* title, const char* reason, bool can_retry)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    textBox(*page_, page_->root(), kBody, reason, font::body(), color::text(), Align::Center);
    if (can_retry) {
        static const char* const kLabels[] = {str::kBtnRetry, str::kBtnExit};
        addButtons(kLabels, 2);
    } else {
        static const char* const kLabels[] = {str::kBtnExit};
        addButtons(kLabels, 1);
    }
}

void View::showDiag(const char* title, const char* body)
{
    LvglLockGuard lock;
    newPage();
    titleBar(*page_, title);
    // 「戻る」はボタン帯に入らない (最大 2 つ) ので頭部タッチ。タイトル帯の右に案内を出す。
    auto& hint = textBox(*page_, page_->root(), Rect{kScreenW - 8 - 180, 0, 180, kTitleH}, str::kDiagBackHint,
                         font::body(), color::text(), Align::Left);
    hint.setTextAlign(LV_TEXT_ALIGN_RIGHT);
    auto& label = textBox(*page_, page_->root(), kBody, body, font::body(), color::text(), Align::Left);
    label_diag_ = label.get();
    static const char* const kLabels[] = {str::kBtnReconnect, str::kBtnShootNoJudge};
    addButtons(kLabels, 2);
}

void View::updateDiagBody(const char* body)
{
    LvglLockGuard lock;
    if (label_diag_ != nullptr) {
        lv_label_set_text(label_diag_, body);
    }
}

}  // namespace photobooth::view
