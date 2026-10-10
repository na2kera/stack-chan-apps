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
};

// 直近 (window_ms 以内) に 2xx を受けていて、連続失敗が上限未満なら online。
bool linkOnline(const LinkSnapshot& s, uint32_t now_ms, uint32_t window_ms);
// 待機画面の 3 状態。Wi-Fi が無ければ Offline。
LinkState linkState(const LinkSnapshot& s, bool wifi_up, uint32_t now_ms, uint32_t window_ms);

}  // namespace photobooth::net::link
