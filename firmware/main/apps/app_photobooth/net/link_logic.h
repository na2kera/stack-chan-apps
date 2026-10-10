/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge との接続状態の判断 (docs/design/step6-cloud-device.md §3.2)。ロジック層: ESP-IDF・FreeRTOS の
// ヘッダを含めない。時刻・エラーの値は呼び出し側 (net/http_edge_client.cpp) が取って渡す。
//
//   - 接続の失敗の分類 (DNS / TCP / 証明書 / 時刻未同期 / TLS)
//   - リクエストごとの 1 試行の期限 (タイムアウト表)
//   - やりとりの結果の分類と、待機画面の 3 状態 (Offline / Starting / Online)
//   - https の前にシステム時刻を待つかどうか
//   - 定期 hello を送るか、hello を送り直すか (net タスクを hello で長く塞がない)
#pragma once

#include <cstdint>

#include "edge_types.h"

namespace photobooth::net::link {

// ---- 時刻 ----

// これより前のシステム時刻は「不正」(RTC が戻っていない・SNTP 未同期)。2025-01-01T00:00:00Z。
inline constexpr int64_t kValidClockEpochSec = 1735689600;
// https で時刻が不正なとき、最初の hello の前に SNTP の同期を待つ上限。
inline constexpr uint32_t kClockWaitMs = 5000;

bool clockValid(int64_t unix_sec);
// 最初の hello の前に時刻の同期を待つか (https で、時刻が不正なときだけ)。
bool shouldWaitForClock(bool https, bool clock_valid);

// ---- 接続の失敗の分類 ----

// esp-tls / mbedTLS の値。ロジック層からヘッダを見ないための写し。一致は接点層で static_assert する。
inline constexpr int kTlsErrCannotResolveHostname = 0x8001;  // ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
inline constexpr int kTlsErrCannotCreateSocket    = 0x8002;  // ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET
inline constexpr int kTlsErrFailedConnectToHost   = 0x8004;  // ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST
inline constexpr int kTlsErrConnectionTimeout     = 0x8006;  // ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT
inline constexpr int kCertFlagExpired             = 0x01;    // MBEDTLS_X509_BADCERT_EXPIRED
inline constexpr int kCertFlagFuture              = 0x0200;  // MBEDTLS_X509_BADCERT_FUTURE

// esp_http_client_open() が失敗したときに取れる値。
struct ConnectError {
    bool https         = false;
    int tls_last_error = 0;  // esp_http_client_get_and_clear_last_tls_error() の戻り値 (esp-tls の last_error)
    int sock_errno     = 0;  // esp_http_client_get_errno()
    int cert_flags     = 0;  // 同上の esp_tls_flags (証明書の検証結果。0 なら問題なし)
    bool clock_valid   = true;
};

enum class ConnectFailure : uint8_t {
    Dns,             // 名前解決できない
    Tcp,             // TCP で繋がらない (拒否・タイムアウト・経路なし。http の不明な失敗もここ)
    Cert,            // 証明書を検証できない
    ClockNotSynced,  // 証明書の期限の検査で落ちたが、原因はこちらの時刻が不正なこと
    Tls,             // その他の TLS の失敗 (ハンドシェイク)
};

ConnectFailure classifyConnectFailure(const ConnectError& e);

// ---- タイムアウト表 ----

enum class RequestKind : uint8_t { Hello, Frame, Candidate, Other };

// 1 試行の期限。Candidate は candidate_timeout_ms (0 なら EDGE_TIMEOUT_MS)。
uint32_t requestTimeoutMs(RequestKind kind, uint32_t candidate_timeout_ms = 0);

// ---- やりとりの結果と接続状態 ----

// 1 回のやりとり (再送したならその最後) で分かったこと。
struct ReplyFacts {
    int http_status       = 0;      // HTTP の status (応答が無ければ 0)
    bool aborted          = false;  // アプリを閉じる途中で打ち切った
    bool connected        = false;  // 接続は成立した (esp_http_client_open() が成功した)
    bool response_timeout = false;  // 接続後、応答 (ヘッダ・本文) の待ちで期限切れ
};

enum class ReplyClass : uint8_t {
    Success,       // 2xx
    Starting,      // 接続は成立したが応答待ちで期限切れ、または 5xx (cold start など)
    Unreachable,   // 接続できない (DNS / TCP / TLS / 証明書)
    Unauthorized,  // 401
    OtherError,    // それ以外の HTTP エラー・途中切断など
    Aborted,
};

ReplyClass classifyReply(const ReplyFacts& f);

// hello の結果を受けて「準備中」かどうかを決め直す。Aborted なら前の値のまま。
bool startingAfterHello(bool prev_starting, ReplyClass c);

// この回数続けて失敗したら offline (フレームを送らず、hello で復帰を待つ)。
inline constexpr uint8_t kOfflineAfterFailures = 3;

struct LinkSnapshot {
    bool ever_ok        = false;  // 一度でも 2xx を受けた
    uint8_t failures    = 0;      // 連続失敗回数
    uint32_t last_ok_ms = 0;      // 最後に 2xx を受けた時刻
    bool starting       = false;  // 直近の hello が「準備中」
    bool hello_settled  = false;  // 最初の hello の結果 (打ち切り以外) が出た。出るまでは Starting
    bool latency_sensitive = false;  // hello が後続の依頼を妨げる段階 (定期 hello を止めている)
};

// 連続失敗が上限未満で、直近 (window_ms 以内) に 2xx を受けていれば online。
// latency_sensitive の間 (判定つきの撮影〜写真の準備完了まで) は定期 hello を送らないので時間の窓は見ず、
// frame などの結果 (連続失敗) だけで決める (REVIEW で迷っている間に online が切れて「撮り直す」が
// 失敗しないように)。写真の準備が終わった後 (QR 表示中) は hello を再開し、時間の窓に戻る。
bool linkOnline(const LinkSnapshot& s, uint32_t now_ms, uint32_t window_ms);
// 待機画面の 3 状態。Wi-Fi が無ければ Offline。net タスクを始めてから最初の hello の結果が出るまで
// (hello_settled でない間。クラウドでは cold start + TLS で 2〜10 秒) は Starting。
LinkState linkState(const LinkSnapshot& s, bool wifi_up, uint32_t now_ms, uint32_t window_ms);

// Starting が連続している時間を測る。Starting 以外を渡すとリセットする。呼び出し側 (Flow) が毎周期
// 今の LinkState を渡し、戻り値 (Starting が続いている ms。Starting でなければ 0) を idleTouch() に渡す。
class StartingClock {
public:
    uint32_t update(LinkState state, uint32_t now_ms);

private:
    bool active_       = false;
    uint32_t since_ms_ = 0;
};

// 待機中のタッチ (画面・頭部) をどう扱うか。
enum class IdleTouch : uint8_t {
    Start,     // 判定つきの撮影を始める (Online)
    Ignore,    // 何もしない (Starting が STARTING_TOUCH_IGNORE_MS 未満: 最初の hello の結果待ち・cold start。予約もしない)
    OpenDiag,  // 診断画面を開く (Offline: DNS / TCP / TLS / 証明書などで繋がらない。Starting が長く続いたときも)
};

// starting_elapsed_ms: Starting が連続している時間 (StartingClock::update() の戻り値)。
IdleTouch idleTouch(LinkState state, uint32_t starting_elapsed_ms);

// net タスクが 2xx を受けたとき、診断の last_error を消すか。DIAG の「再接続」で置いた「再接続中」
// (str::kNetReconnecting と完全一致) だけを消し、他のエラーは「最後の通信エラー」として残す。
bool clearErrorOnSuccess(const char* last_error);

// ---- 定期 hello ----

struct HelloGate {
    bool wifi_up           = false;
    bool quitting          = false;  // アプリを閉じる途中
    bool latency_sensitive = false;  // hello が後続の timeout / candidate / save / photo を妨げる段階
    bool commands_waiting  = false;  // コマンドキューに依頼が残っている
    bool requested_once    = false;  // このタスクで一度でもリクエストを送った
    uint32_t since_last_ms = 0;      // 最後のリクエストからの経過
    uint32_t interval_ms   = 0;      // HELLO_INTERVAL_MS
};

// 定期 hello を今送るか。hello は 1 試行最長 HELLO_TIMEOUT_MS で net タスクを塞ぐので、latency_sensitive の間と
// 依頼が残っているときは送らない (その間の接続状態は frame などの結果で分かる)。
bool shouldSendHello(const HelloGate& g);

// hello の 1 回目が通信失敗のとき 2 回目を送るか。依頼が並んでいれば送らない (依頼を待たせない)。
bool helloRetryAllowed(bool commands_waiting);

}  // namespace photobooth::net::link
