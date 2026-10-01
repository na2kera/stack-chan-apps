/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 画面に出す日本語文言 (docs/spec.md §4, docs/design/step1-device.md §4/§5 のまま)。
//
// 日本語フォント assets/pb_font_jp_20.c はこのファイルの文字列リテラルに含まれる文字だけから
// 作っている。文言を変えたら tools/gen_font.sh を実行してフォントを作り直すこと
// (作り直さないと、増えた文字が表示されない)。
#pragma once

namespace photobooth::str {

// ---- タイトル帯 (状態名) ----
inline constexpr const char* kTitleIdle      = "待機中";
inline constexpr const char* kTitleAnnounce  = "撮影開始";
inline constexpr const char* kTitleCompose   = "構図あわせ";
inline constexpr const char* kTitleCapture   = "撮影中";
inline constexpr const char* kTitleReview    = "確認";
inline constexpr const char* kTitleUploading = "準備中";
inline constexpr const char* kTitlePhotoQr   = "写真を保存";
inline constexpr const char* kTitleXQr       = "Xに投稿";
inline constexpr const char* kTitleError     = "エラー";

// ---- 本文 ----
inline constexpr const char* kIdlePrompt     = "写真を撮りたい、と言ってね";
inline constexpr const char* kIdleTouchStart = "タッチで開始";
inline constexpr const char* kPcOffline      = "PC未接続";
inline constexpr const char* kAnnounce       = "写真を撮るよ！ いい顔をしてね";
inline constexpr const char* kCompose        = "みんな画面に入ってね";
inline constexpr const char* kFaceCountLabel = "人数";
inline constexpr const char* kFaceCountNone  = "--";
inline constexpr const char* kNoCandidate    = "候補の写真がありません";
inline constexpr const char* kUploading      = "写真を準備中";
inline constexpr const char* kExpiresLabel   = "削除予定";
inline constexpr const char* kExpiresUnknown = "--:--";
inline constexpr const char* kXQrGuide       = "保存した写真をXに添付してね";
inline constexpr const char* kCameraDisabled = "カメラ無効";

// ---- ボタン ----
inline constexpr const char* kBtnSave  = "保存する";
inline constexpr const char* kBtnRetake = "撮り直す";
inline constexpr const char* kBtnNext  = "次へ";
inline constexpr const char* kBtnBack  = "戻る";
inline constexpr const char* kBtnExit  = "終了";
inline constexpr const char* kBtnRetry = "再試行";

// ---- エラー・警告 ----
inline constexpr const char* kErrCameraInit    = "カメラ初期化失敗";
inline constexpr const char* kErrCameraNoFrame = "カメラからフレームを取得できません";
inline constexpr const char* kErrCameraBusy    = "カメラを再起動できません";
inline constexpr const char* kErrNoMemory      = "メモリ不足";
inline constexpr const char* kWarnHeadFault    = "首モーター応答なし";

}  // namespace photobooth::str
