/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 画面に出す日本語文言 (docs/spec.md §4, docs/design/step1-device.md §4/§5, fw-app-step2.md §4)。
// 通信層 (net/) が診断画面・ERROR 画面に出す文言もここに置く (フォントに含めるため)。
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
inline constexpr const char* kTitleShutter   = "撮れたよ";  // SHUTTER: 撮れた写真に重ねるタイトル帯
inline constexpr const char* kTitleReview    = "確認";
inline constexpr const char* kTitleUploading = "準備中";
inline constexpr const char* kTitlePhotoQr   = "写真を保存";  // QR 画面のタイトルと左 QR の見出し
inline constexpr const char* kTitleXQr       = "Xに投稿";     // QR 画面の右 QR の見出し
inline constexpr const char* kTitleError     = "エラー";
inline constexpr const char* kTitleDiag      = "接続診断";

// ---- 本文 ----
inline constexpr const char* kIdlePrompt     = "写真を撮りたい、と言ってね";
inline constexpr const char* kIdleTouchStart = "タッチで開始";
// 待機画面の右下の接続状態 (net::LinkState。docs/design/step6-cloud-device.md §3.2「UI の状態」)。
// 枠は 100x24 px (1 行) なので日本語 5 文字以内にする (6 文字以上は折り返して 2 行目が切れる)。
inline constexpr const char* kLinkOffline    = "接続なし";
inline constexpr const char* kLinkStarting   = "準備中";
inline constexpr const char* kLinkOnline     = "接続中";
inline constexpr const char* kWifiConnecting = "Wi-Fi接続中";  // アプリを開いた直後、純正の startNetwork() を待つ画面
inline constexpr const char* kAnnounce       = "写真を撮るよ！ いい顔をしてね";
inline constexpr const char* kCompose        = "みんな画面に入ってね";
inline constexpr const char* kBandCloser     = "もう少し寄ってね";
inline constexpr const char* kBandTooManySuffix = "人までだよ";  // 「4人までだよ」(人数は config::MAX_FACES)
inline constexpr const char* kFaceCountLabel = "人数";
inline constexpr const char* kFaceCountNone  = "--";
inline constexpr const char* kNoCandidate    = "候補の写真がありません";  // 顔の有無が分からないとき
inline constexpr const char* kNoFace         = "顔が見つからなかったよ";  // spec §9: 顔なしで時間切れ
// edge に候補はあるが受け取れない・表示できないとき (保存はさせない)
inline constexpr const char* kCandidateUnavailable = "候補の写真を表示できません";
inline constexpr const char* kCaptured       = "撮れたよ";
inline constexpr const char* kUploading      = "写真を準備中";
inline constexpr const char* kExpiresLabel   = "削除予定";
inline constexpr const char* kExpiresUnknown = "--:--";
inline constexpr const char* kCameraDisabled = "カメラ無効";

// ---- ボタン ----
inline constexpr const char* kBtnRetake = "撮り直す";
inline constexpr const char* kBtnNext  = "次へ";
inline constexpr const char* kBtnExit  = "終了";
inline constexpr const char* kBtnRetry = "再試行";
inline constexpr const char* kBtnReconnect = "再接続";
inline constexpr const char* kBtnShootNoJudge = "判定なしで撮影";

// ---- エラー・警告 ----
inline constexpr const char* kErrCameraInit    = "カメラ初期化失敗";
inline constexpr const char* kErrCameraNoFrame = "カメラからフレームを取得できません";
inline constexpr const char* kErrCameraBusy    = "カメラを再起動できません";
inline constexpr const char* kErrNoMemory      = "メモリ不足";
inline constexpr const char* kWarnHeadFault    = "首モーター応答なし";
inline constexpr const char* kErrEdgeLost      = "接続が切れました";
inline constexpr const char* kErrNoPc          = "接続が無いため保存できません";
inline constexpr const char* kErrUploadFailed  = "写真を保存できませんでした";
inline constexpr const char* kErrRetryExhausted = "再試行回数を超えました";
inline constexpr const char* kErrQr             = "QRを表示できません";
inline constexpr const char* kErrRetakeGuide   = "終了して撮り直してね";

// ---- 診断画面 (DIAG) ----
inline constexpr const char* kDiagBackHint     = "頭タッチで戻る";
inline constexpr const char* kDiagWifiLabel    = "Wi-Fi";
inline constexpr const char* kDiagPcLabel      = "接続先";
inline constexpr const char* kDiagReplyLabel   = "応答";
inline constexpr const char* kDiagErrorLabel   = "エラー";
inline constexpr const char* kDiagWifiConnected    = "接続済み";
inline constexpr const char* kDiagWifiDisconnected = "未接続";
inline constexpr const char* kDiagWifiNotConfigured = "未設定";
inline constexpr const char* kDiagWifiConfigMode   = "設定モード";
inline constexpr const char* kDiagReplyYes     = "あり";
inline constexpr const char* kDiagReplyNo      = "なし";
inline constexpr const char* kDiagNone         = "なし";

// ---- 通信層 (net/) の短い説明。診断画面の「エラー」と ERROR 画面に出る ----
inline constexpr const char* kNetNoWifi        = "Wi-Fi未接続";
inline constexpr const char* kNetWifiNotConfigured = "Wi-Fiが未設定です (SETUPで設定)";
inline constexpr const char* kNetBodyTooLarge  = "応答が大きすぎます";
inline constexpr const char* kNetBadJson       = "応答を読めません";
inline constexpr const char* kNetMismatch      = "応答が要求と合いません";
inline constexpr const char* kNetConnectFailed = "接続できません";
inline constexpr const char* kNetSendFailed    = "送信に失敗しました";
inline constexpr const char* kNetLost          = "接続が切れました";
inline constexpr const char* kNetNoResponse    = "応答がありません";
inline constexpr const char* kNetDnsFailed     = "DNS失敗";
inline constexpr const char* kNetCertError     = "証明書エラー";
inline constexpr const char* kNetClockNotSynced = "時刻未同期";
inline constexpr const char* kNetTlsFailed     = "TLS接続失敗";
inline constexpr const char* kNetBadUrl        = "接続先の書式が不正";
inline constexpr const char* kNetStartingWait  = "準備中 (応答待ち)";
inline constexpr const char* kNetUnauthorized  = "認証エラー(IDか鍵が違う)";
inline constexpr const char* kNetError         = "通信エラー";
inline constexpr const char* kNetQueueFull     = "コマンドが溢れました";
inline constexpr const char* kNetReconnecting  = "再接続中";
inline constexpr const char* kNetPhotoTimeout  = "写真の準備が時間内に終わりません";
inline constexpr const char* kNetBadPhotoUrl   = "写真URLが不正です";
inline constexpr const char* kNetTaskFailed    = "通信タスクを起動できません";
inline constexpr const char* kNetTaskBusy      = "前の通信が終わっていません (再接続を押してね)";
inline constexpr const char* kNetDisabledBuild = "edge連携なしのビルド";

}  // namespace photobooth::str
