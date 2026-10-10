/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "link_logic.h"

#include <cerrno>

#include "../config.h"

namespace photobooth::net::link {

bool clockValid(int64_t unix_sec)
{
    return unix_sec >= kValidClockEpochSec;
}

bool shouldWaitForClock(bool https, bool clock_valid)
{
    return https && !clock_valid;
}

ConnectFailure classifyConnectFailure(const ConnectError& e)
{
    if (e.tls_last_error == kTlsErrCannotResolveHostname) {
        return ConnectFailure::Dns;
    }
    if (e.cert_flags != 0) {
        // 期限切れ・未来の証明書は、こちらの時刻が不正なら時刻のせい。時刻が妥当なら本当に証明書の問題。
        if (!e.clock_valid && (e.cert_flags & (kCertFlagExpired | kCertFlagFuture)) != 0) {
            return ConnectFailure::ClockNotSynced;
        }
        return ConnectFailure::Cert;
    }
    if (e.sock_errno == ECONNREFUSED || e.sock_errno == ETIMEDOUT || e.sock_errno == EHOSTUNREACH ||
        e.sock_errno == ENETUNREACH) {
        return ConnectFailure::Tcp;
    }
    if (e.tls_last_error == kTlsErrFailedConnectToHost || e.tls_last_error == kTlsErrConnectionTimeout ||
        e.tls_last_error == kTlsErrCannotCreateSocket) {
        return ConnectFailure::Tcp;
    }
    if (e.https && e.tls_last_error != 0) {
        return ConnectFailure::Tls;
    }
    return ConnectFailure::Tcp;
}

uint32_t requestTimeoutMs(RequestKind kind, uint32_t candidate_timeout_ms)
{
    switch (kind) {
        case RequestKind::Hello:
            return config::HELLO_TIMEOUT_MS;
        case RequestKind::Candidate:
            return candidate_timeout_ms != 0 ? candidate_timeout_ms : config::EDGE_TIMEOUT_MS;
        case RequestKind::Frame:
        case RequestKind::Other:
            break;
    }
    return config::EDGE_TIMEOUT_MS;
}

ReplyClass classifyReply(const ReplyFacts& f)
{
    if (f.aborted) {
        return ReplyClass::Aborted;
    }
    if (f.http_status > 0) {
        if (f.http_status >= 200 && f.http_status < 300) {
            return ReplyClass::Success;
        }
        if (f.http_status == 401) {
            return ReplyClass::Unauthorized;
        }
        if (f.http_status >= 500) {
            return ReplyClass::Starting;
        }
        return ReplyClass::OtherError;
    }
    if (!f.connected) {
        return ReplyClass::Unreachable;
    }
    return f.response_timeout ? ReplyClass::Starting : ReplyClass::OtherError;
}

bool startingAfterHello(bool prev_starting, ReplyClass c)
{
    switch (c) {
        case ReplyClass::Starting:
            return true;
        case ReplyClass::Aborted:
            return prev_starting;
        case ReplyClass::Success:
        case ReplyClass::Unreachable:
        case ReplyClass::Unauthorized:
        case ReplyClass::OtherError:
            break;
    }
    return false;
}

bool linkOnline(const LinkSnapshot& s, uint32_t now_ms, uint32_t window_ms)
{
    if (!s.ever_ok || s.failures >= kOfflineAfterFailures) {
        return false;
    }
    if (s.latency_sensitive) {
        return true;
    }
    return now_ms - s.last_ok_ms < window_ms;  // millis の一周をまたいでも差は正しい
}

LinkState linkState(const LinkSnapshot& s, bool wifi_up, uint32_t now_ms, uint32_t window_ms)
{
    if (!wifi_up) {
        return LinkState::Offline;
    }
    if (linkOnline(s, now_ms, window_ms)) {
        return LinkState::Online;
    }
    return s.starting ? LinkState::Starting : LinkState::Offline;
}

bool shouldSendHello(const HelloGate& g)
{
    if (!g.wifi_up || g.quitting || g.latency_sensitive || g.commands_waiting) {
        return false;
    }
    return !g.requested_once || g.since_last_ms >= g.interval_ms;
}

bool helloRetryAllowed(bool commands_waiting)
{
    return !commands_waiting;
}

}  // namespace photobooth::net::link
