/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 状態ごとの画面 (docs/design/step1-device.md §5 ui を LVGL に移植。ステップ2 の追加は fw-app-step2.md §4)。
//
// show*() は状態に入ったときに 1 回だけ呼んで画面を作り直す。変化する部分 (プレビュー、残り秒数) は
// update*() で差し替える。どのメソッドも中で LvglLockGuard を取るので、呼び出し側は取らないこと。
// 画面のボタン・タップは shared::hw::Input に (画面の世代番号付きで) 積む。
#pragma once

#include <lvgl.h>

#include <cstdint>
#include <memory>
#include <string_view>

class LvglAllocatedImage;  // 純正の JPEG デコード結果 (display/lvgl_display/lvgl_image.h)

namespace shared::hw {
class Input;
}

namespace photobooth::view {

class Page;

// SHUTTER で撮れた写真として何を出したか。
enum class CapturedSource : uint8_t {
    Candidate,  // edge の候補 JPEG (prepareCapturedJpeg() で用意したもの)
    Preview,    // 最後のプレビューフレーム (止めたまま)
    None,       // どちらも無い (タイトル帯だけ)
};

// 待機画面の右下に出す接続状態。
enum class IdleLink : uint8_t {
    Offline,   // 「接続なし」
    Starting,  // 「準備中」(edge の起動待ち)
    Online,    // 「接続中」
};

class View {
public:
    explicit View(shared::hw::Input& input);
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

    // アプリを開いた直後: 「Wi-Fi接続中」。純正の startNetwork() が繋がるまでの間だけ出す (ボタンなし)。
    void showWifiConnecting();
    // その画面の進み具合の 1 行 (純正の onLog の文言) を差し替える。Wi-Fi のイベントタスクから呼ばれる。
    void setWifiMessage(std::string_view text);
    // 待機: 案内文 + 「タッチで開始」+「終了」。画面のどこを触っても開始 (ボタン以外)。
    // 右下に接続状態 (「PC接続中」/「PC未接続」)、warning があれば左下に警告を出す。
    void showIdle(IdleLink link, const char* warning);
    // 右下の接続状態だけを差し替える。
    void updateIdleStatus(IdleLink link);
    // ANNOUNCE
    void showAnnounce(const char* title);
    // COMPOSE: プレビュー + 下端の案内帯 (既定は「みんな画面に入ってね」)。camera_ok=false ならプレビュー枠だけ。
    void showCompose(bool camera_ok);
    // CAPTURE: プレビュー + 右上に人数と残り秒数。案内帯は setBand() で出すまで隠れている。
    void showCapture(bool camera_ok);
    // COMPOSE / CAPTURE の下端の案内帯の文言を差し替える。nullptr なら帯を隠す。
    void setBand(const char* text);
    // 右上の「人数 face/target」と残り秒数。face_count < 0 は「人数 --」、target <= 0 は「人数 face/--」。
    void updateCaptureOverlay(int remaining_sec, int face_count, int target);
    // COMPOSE / CAPTURE のプレビューを差し替える (RGB565 LE)。
    void updatePreview(const uint16_t* pixels, int width, int height);
    // REVIEW: 候補フレーム + 「撮り直す」「次へ」。pixels が nullptr、または表示用バッファが無くて
    // 出せなかったときは「撮り直す」だけ。候補を出せたら true (ボタン 0 =「撮り直す」、1 =「次へ」)、
    // 出せなかったら false (ボタン 0 =「撮り直す」)。
    bool showReview(const char* title, const uint16_t* pixels, int width, int height);
    // REVIEW: edge の候補 JPEG を全面に出し、タイトル帯と「撮り直す」「次へ」を重ねる。
    // ヘッダの大きさが 320x240 でない、またはデコードできなければ、画面を変えずに false。
    bool showReviewJpeg(const char* title, const uint8_t* jpeg, size_t len);
    // SHUTTER: 全面を白くする (フラッシュ)。ボタンなし。プレビュー用バッファの中身は変えない。
    void showShutterFlash();
    // SHUTTER: edge の候補 JPEG を確かめてデコードし、表示せずに持っておく (showReviewJpeg と同じ検査)。
    // 今の画面は変えない。デコードできなければ false。
    bool prepareCapturedJpeg(const uint8_t* jpeg, size_t len);
    // SHUTTER: 撮れた写真を全面に出し、タイトル帯 (title) を重ねる。ボタンなし。
    // prepareCapturedJpeg() で用意した JPEG があればそれを (所有権は画面へ移る)、無ければ最後の
    // プレビューフレームを止めたまま出す。どちらを出したかを返す。
    CapturedSource showCaptured(const char* title);
    // prepareCapturedJpeg() で用意したまま表示しなかった JPEG を解放する。
    void discardCapturedJpeg();
    // REVIEW: 候補なし。text (「顔が見つからなかったよ」など) と「撮り直す」だけ。
    void showReviewEmpty(const char* title, const char* text);
    // UPLOADING: 「写真を準備中」。captured=true なら上に「撮れたよ」を出す (自動採用のとき)。
    void showUploading(const char* title, bool captured);
    // QR: タイトル帯 (右側に削除予定時刻) + 写真 QR (左) と投稿 QR (右) + 「撮り直す」「終了」
    // (docs/design/ui-review-two-qr.md)。どちらかの QR を作れなければ false
    // (呼び出し側が ERROR 画面に切り替える)。
    bool showQr(const char* title, const char* photo_url, const char* share_url, const char* expires_at);
    // ERROR: 理由 + 「再試行」「終了」。can_retry=false なら「終了」だけ (ボタン 0 =「終了」)。
    void showError(const char* title, const char* reason, bool can_retry = true);
    // DIAG: 診断 (複数行、'\n' 区切り) + 「再接続」「判定なしで撮影」。戻るは頭部タッチ。
    void showDiag(const char* title, const char* body);
    // 診断の本文だけを差し替える。
    void updateDiagBody(const char* body);

private:
    void newPage();
    void addButtons(const char* const* labels, int count);
    void addPreview(bool camera_ok);
    void addBand(const char* text, bool hidden);
    bool setPreviewSize(int width, int height);
    // ヘッダを確かめて (320x240 のベースラインだけ) RGB565 にデコードする。LVGL のロックの外で呼ぶ。
    static std::shared_ptr<LvglAllocatedImage> decodeCandidateJpeg(const uint8_t* jpeg, size_t len);

    shared::hw::Input& input_;
    std::unique_ptr<Page> page_;
    uint32_t screen_id_ = 0;

    uint16_t* preview_buf_    = nullptr;  // PSRAM。lv_image が直接参照する
    size_t preview_capacity_  = 0;        // 画素数
    lv_image_dsc_t preview_dsc_{};
    lv_obj_t* preview_img_    = nullptr;  // 今の画面のプレビュー (無ければ nullptr)
    bool preview_has_frame_   = false;    // preview_buf_ にカメラのフレームが入っているか (SHUTTER で使う)
    lv_obj_t* label_faces_    = nullptr;  // CAPTURE の人数
    lv_obj_t* label_remain_   = nullptr;  // CAPTURE の残り秒数
    lv_obj_t* label_status_   = nullptr;  // 待機の接続状態
    lv_obj_t* band_panel_     = nullptr;  // COMPOSE / CAPTURE の案内帯
    lv_obj_t* band_label_     = nullptr;
    lv_obj_t* label_diag_     = nullptr;  // DIAG の本文
    lv_obj_t* label_wifi_     = nullptr;  // 「Wi-Fi接続中」画面の進み具合
    // REVIEW に出している候補 JPEG のデコード結果。lv_image が参照するので、画面を作り直すまで持つ。
    std::shared_ptr<LvglAllocatedImage> review_image_;
    // SHUTTER で表示を待っている候補 JPEG のデコード結果 (まだ lv_image は参照していない)。
    std::shared_ptr<LvglAllocatedImage> captured_image_;
};

}  // namespace photobooth::view
