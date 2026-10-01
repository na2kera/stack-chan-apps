/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 状態ごとの画面 (docs/design/step1-device.md §5 ui を LVGL に移植)。
//
// show*() は状態に入ったときに 1 回だけ呼んで画面を作り直す。変化する部分 (プレビュー、残り秒数) は
// update*() で差し替える。どのメソッドも中で LvglLockGuard を取るので、呼び出し側は取らないこと。
// 画面のボタン・タップは hw::Input に (画面の世代番号付きで) 積む。
#pragma once

#include <lvgl.h>

#include <cstdint>
#include <memory>

namespace photobooth::hw {
class Input;
}

namespace photobooth::view {

class Page;

class View {
public:
    explicit View(hw::Input& input);
    ~View();

    // プレビュー用の RGB565 バッファ (PSRAM) を確保する。
    bool begin();
    // 画面を消してバッファを解放する。
    void end();

    // 今表示している画面の世代番号。古い画面で押されたボタンを無視するのに使う。
    uint32_t screenId() const
    {
        return screen_id_;
    }

    // 待機: 案内文 + 「タッチで開始」+「終了」。画面のどこを触っても開始 (ボタン以外)。
    // pc_online=false なら「PC未接続」、warning があれば警告を出す。
    void showIdle(bool pc_online, const char* warning);
    // ANNOUNCE
    void showAnnounce(const char* title);
    // COMPOSE: プレビュー + 下端に「みんな画面に入ってね」。camera_ok=false ならプレビュー枠だけ。
    void showCompose(bool camera_ok);
    // CAPTURE: プレビュー + 右上に人数と残り秒数。
    void showCapture(bool camera_ok);
    void updateCaptureOverlay(int remaining_sec, int face_count);
    // COMPOSE / CAPTURE のプレビューを差し替える (RGB565 LE)。
    void updatePreview(const uint16_t* pixels, int width, int height);
    // REVIEW: 候補フレーム + 「保存する」「撮り直す」。pixels が nullptr、または表示用バッファが無くて
    // 出せなかったときは「撮り直す」だけ。候補を出せたら true (ボタン 0 =「保存する」)、
    // 出せなかったら false (ボタン 0 =「撮り直す」)。
    bool showReview(const char* title, const uint16_t* pixels, int width, int height);
    // UPLOADING
    void showUploading(const char* title);
    // PHOTO_QR: 写真 QR + 削除予定時刻 + 「次へ」「撮り直す」
    void showPhotoQr(const char* title, const char* photo_url, const char* expires_at);
    // X_QR: 投稿 QR + 案内 + 「戻る」「終了」
    void showXQr(const char* title, const char* share_url);
    // ERROR: 理由 + 「再試行」「終了」
    void showError(const char* title, const char* reason);

private:
    void newPage();
    void addButtons(const char* const* labels, int count);
    void addPreview(bool camera_ok);
    bool setPreviewSize(int width, int height);

    hw::Input& input_;
    std::unique_ptr<Page> page_;
    uint32_t screen_id_ = 0;

    uint16_t* preview_buf_    = nullptr;  // PSRAM。lv_image が直接参照する
    size_t preview_capacity_  = 0;        // 画素数
    lv_image_dsc_t preview_dsc_{};
    lv_obj_t* preview_img_    = nullptr;  // 今の画面のプレビュー (無ければ nullptr)
    lv_obj_t* label_faces_    = nullptr;  // CAPTURE の人数
    lv_obj_t* label_remain_   = nullptr;  // CAPTURE の残り秒数
};

}  // namespace photobooth::view
