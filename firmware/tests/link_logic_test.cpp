/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の net/link_logic (接続の失敗の分類、タイムアウト表、Offline / Starting / Online。
// docs/design/step6-cloud-device.md §3.2) をホストで確かめる。
#include <apps/app_photobooth/config.h>
#include <apps/app_photobooth/net/link_logic.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

namespace link = photobooth::net::link;
namespace config = photobooth::config;
using photobooth::net::LinkState;

void expectEqual(long long actual, long long expected, const char* label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void expectTrue(bool value, const char* label)
{
    if (!value) {
        std::cerr << "FAILED: " << label << '\n';
        std::exit(1);
    }
}

void expectFailure(const link::ConnectError& e, link::ConnectFailure expected, const char* label)
{
    expectEqual(static_cast<int>(link::classifyConnectFailure(e)), static_cast<int>(expected), label);
}

void expectClass(const link::ReplyFacts& f, link::ReplyClass expected, const char* label)
{
    expectEqual(static_cast<int>(link::classifyReply(f)), static_cast<int>(expected), label);
}

void expectState(LinkState actual, LinkState expected, const char* label)
{
    expectEqual(static_cast<int>(actual), static_cast<int>(expected), label);
}

void testClock()
{
    expectTrue(!link::clockValid(0), "1970 is invalid");
    expectTrue(!link::clockValid(link::kValidClockEpochSec - 1), "2024-12-31T23:59:59Z is invalid");
    expectTrue(link::clockValid(link::kValidClockEpochSec), "2025-01-01T00:00:00Z is valid");
    expectTrue(link::clockValid(1791590400), "2026-10 is valid");
    expectTrue(link::shouldWaitForClock(true, false), "https + invalid clock waits");
    expectTrue(!link::shouldWaitForClock(true, true), "https + valid clock does not wait");
    expectTrue(!link::shouldWaitForClock(false, false), "http never waits");
    expectEqual(link::kClockWaitMs, 5000, "clock wait limit");
}

void testConnectFailure()
{
    link::ConnectError e;
    e.https          = true;
    e.tls_last_error = link::kTlsErrCannotResolveHostname;
    expectFailure(e, link::ConnectFailure::Dns, "dns (https)");
    e.https = false;
    expectFailure(e, link::ConnectFailure::Dns, "dns (http)");

    // TCP: errno で分かる
    for (int err : {ECONNREFUSED, ETIMEDOUT, EHOSTUNREACH, ENETUNREACH}) {
        link::ConnectError t;
        t.https          = true;
        t.tls_last_error = link::kTlsErrFailedConnectToHost;
        t.sock_errno     = err;
        expectFailure(t, link::ConnectFailure::Tcp, "tcp errno");
    }
    // TCP: errno が無くても esp-tls の接続失敗・接続タイムアウトなら TCP
    link::ConnectError t;
    t.https          = true;
    t.tls_last_error = link::kTlsErrConnectionTimeout;
    expectFailure(t, link::ConnectFailure::Tcp, "tcp connection timeout");
    t.tls_last_error = link::kTlsErrFailedConnectToHost;
    expectFailure(t, link::ConnectFailure::Tcp, "tcp failed to connect");

    // 証明書: 時刻が妥当なら期限切れも「証明書エラー」
    link::ConnectError c;
    c.https          = true;
    c.tls_last_error = 0x801A;  // ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED
    c.cert_flags     = link::kCertFlagExpired;
    c.clock_valid    = true;
    expectFailure(c, link::ConnectFailure::Cert, "expired cert with valid clock");
    c.cert_flags = 0x08;  // MBEDTLS_X509_BADCERT_NOT_TRUSTED (self-signed)
    expectFailure(c, link::ConnectFailure::Cert, "untrusted cert");
    c.clock_valid = false;
    expectFailure(c, link::ConnectFailure::Cert, "untrusted cert with invalid clock is still cert");
    // 時刻未同期: 期限の検査で落ち、かつ時刻が不正
    c.cert_flags = link::kCertFlagFuture;
    expectFailure(c, link::ConnectFailure::ClockNotSynced, "future cert with invalid clock");
    c.cert_flags = link::kCertFlagExpired | 0x08;
    expectFailure(c, link::ConnectFailure::ClockNotSynced, "expired + other with invalid clock");
    c.clock_valid = true;
    expectFailure(c, link::ConnectFailure::Cert, "future/expired with valid clock");

    // その他の TLS の失敗
    link::ConnectError h;
    h.https          = true;
    h.tls_last_error = 0x801A;
    expectFailure(h, link::ConnectFailure::Tls, "handshake failure");
    // http で分からない失敗は従来どおり TCP (「接続できません」)
    link::ConnectError u;
    u.https = false;
    expectFailure(u, link::ConnectFailure::Tcp, "http unknown");
    u.tls_last_error = 0x801A;
    expectFailure(u, link::ConnectFailure::Tcp, "http with tls code");
}

void testTimeouts()
{
    expectEqual(config::HELLO_TIMEOUT_MS, 8000, "HELLO_TIMEOUT_MS");
    expectEqual(config::EDGE_TIMEOUT_MS, 3000, "EDGE_TIMEOUT_MS");
    expectEqual(config::SHUTTER_CANDIDATE_TIMEOUT_MS, 1500, "SHUTTER_CANDIDATE_TIMEOUT_MS");
    expectEqual(link::requestTimeoutMs(link::RequestKind::Hello), 8000, "hello timeout");
    expectEqual(link::requestTimeoutMs(link::RequestKind::Frame), 3000, "frame timeout");
    expectEqual(link::requestTimeoutMs(link::RequestKind::Other), 3000, "other timeout");
    expectEqual(link::requestTimeoutMs(link::RequestKind::Candidate), 3000, "candidate (review) timeout");
    expectEqual(link::requestTimeoutMs(link::RequestKind::Candidate, config::SHUTTER_CANDIDATE_TIMEOUT_MS), 1500,
                "candidate (shutter) timeout");
}

void testReplyClass()
{
    link::ReplyFacts f;
    f.connected = true;
    for (int s : {200, 201, 204, 299}) {
        f.http_status = s;
        expectClass(f, link::ReplyClass::Success, "2xx");
    }
    f.http_status = 401;
    expectClass(f, link::ReplyClass::Unauthorized, "401");
    for (int s : {500, 502, 503, 504}) {
        f.http_status = s;
        expectClass(f, link::ReplyClass::Starting, "5xx");
    }
    for (int s : {400, 404, 409, 411, 413, 300}) {
        f.http_status = s;
        expectClass(f, link::ReplyClass::OtherError, "other http");
    }

    link::ReplyFacts n;
    expectClass(n, link::ReplyClass::Unreachable, "not connected");
    n.response_timeout = true;  // 接続していないなら応答待ちの期限切れは起きないが、起きても接続失敗の扱い
    expectClass(n, link::ReplyClass::Unreachable, "not connected + timeout");
    n.connected = true;
    expectClass(n, link::ReplyClass::Starting, "connected + response timeout");
    n.response_timeout = false;
    expectClass(n, link::ReplyClass::OtherError, "connected + lost");
    n.aborted = true;
    expectClass(n, link::ReplyClass::Aborted, "aborted wins");
    n.http_status = 200;
    expectClass(n, link::ReplyClass::Aborted, "aborted wins over status");
}

void testStartingAfterHello()
{
    expectTrue(link::startingAfterHello(false, link::ReplyClass::Starting), "starting");
    expectTrue(!link::startingAfterHello(true, link::ReplyClass::Success), "success clears");
    expectTrue(!link::startingAfterHello(true, link::ReplyClass::Unreachable), "unreachable clears");
    expectTrue(!link::startingAfterHello(true, link::ReplyClass::Unauthorized), "401 clears");
    expectTrue(!link::startingAfterHello(true, link::ReplyClass::OtherError), "other clears");
    expectTrue(link::startingAfterHello(true, link::ReplyClass::Aborted), "aborted keeps true");
    expectTrue(!link::startingAfterHello(false, link::ReplyClass::Aborted), "aborted keeps false");
}

void testLinkState()
{
    constexpr uint32_t kWindow = 10000;
    link::LinkSnapshot s;
    expectState(link::linkState(s, true, 0, kWindow), LinkState::Offline, "never ok");
    s.starting = true;
    expectState(link::linkState(s, true, 0, kWindow), LinkState::Starting, "never ok + starting");
    expectState(link::linkState(s, false, 0, kWindow), LinkState::Offline, "no wifi");

    s.starting   = false;
    s.ever_ok    = true;
    s.last_ok_ms = 1000;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Online, "just ok");
    expectState(link::linkState(s, true, 1000 + kWindow - 1, kWindow), LinkState::Online, "window edge - 1");
    expectState(link::linkState(s, true, 1000 + kWindow, kWindow), LinkState::Offline, "window edge");
    s.starting = true;
    expectState(link::linkState(s, true, 1000 + kWindow, kWindow), LinkState::Starting, "lapsed + starting");
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Online, "online wins over starting");
    expectState(link::linkState(s, false, 1000, kWindow), LinkState::Offline, "no wifi wins");

    s.starting = false;
    s.failures = link::kOfflineAfterFailures - 1;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Online, "failures below limit");
    s.failures = link::kOfflineAfterFailures;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Offline, "failures at limit");
    s.starting = true;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Starting, "failures at limit + starting");

    // millis の一周をまたぐ
    link::LinkSnapshot w;
    w.ever_ok    = true;
    w.last_ok_ms = 0xFFFFFF00u;
    expectTrue(link::linkOnline(w, 0x00000100u, kWindow), "wraparound inside window");
    expectTrue(!link::linkOnline(w, 0xFFFFFF00u + kWindow, kWindow), "wraparound at window");
}

void testLatencySensitiveKeepsOnline()
{
    constexpr uint32_t kWindow = 10000;
    link::LinkSnapshot s;
    s.ever_ok        = true;
    s.last_ok_ms     = 1000;
    s.latency_sensitive = true;
    // 撮影〜写真の準備完了の間は定期 hello を止めるので、時間が経っても失敗が無ければ online のまま (REVIEW で迷っている間)
    expectState(link::linkState(s, true, 1000 + kWindow * 6, kWindow), LinkState::Online, "latency sensitive: long idle");
    s.failures = link::kOfflineAfterFailures;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Offline, "latency sensitive: failures still offline");
    s.failures = 0;
    expectState(link::linkState(s, false, 1000, kWindow), LinkState::Offline, "latency sensitive: no wifi");
    s.ever_ok = false;
    expectState(link::linkState(s, true, 1000, kWindow), LinkState::Offline, "latency sensitive: never ok");
    // 写真の準備が終わった後 (QR 表示中) とセッションの後は時間の窓に戻る
    s.ever_ok        = true;
    s.latency_sensitive = false;
    expectState(link::linkState(s, true, 1000 + kWindow, kWindow), LinkState::Offline, "after photo ready (not latency sensitive): window");
}

void testShouldSendHello()
{
    link::HelloGate g;
    g.wifi_up     = true;
    g.interval_ms = 5000;
    expectTrue(link::shouldSendHello(g), "first hello right away");
    g.requested_once = true;
    g.since_last_ms  = 4999;
    expectTrue(!link::shouldSendHello(g), "before interval");
    g.since_last_ms = 5000;
    expectTrue(link::shouldSendHello(g), "at interval");
    g.since_last_ms = 60000;
    expectTrue(link::shouldSendHello(g), "long after");

    link::HelloGate s = g;
    s.latency_sensitive = true;
    expectTrue(!link::shouldSendHello(s), "no hello while latency sensitive (shooting to photo ready)");
    s.requested_once = false;
    expectTrue(!link::shouldSendHello(s), "no hello while latency sensitive even before the first request");

    link::HelloGate c = g;
    c.commands_waiting = true;
    expectTrue(!link::shouldSendHello(c), "no hello while commands wait");

    link::HelloGate w = g;
    w.wifi_up = false;
    expectTrue(!link::shouldSendHello(w), "no hello without wifi");

    link::HelloGate q = g;
    q.quitting = true;
    expectTrue(!link::shouldSendHello(q), "no hello while quitting");

    expectTrue(link::helloRetryAllowed(false), "retry when the queue is empty");
    expectTrue(!link::helloRetryAllowed(true), "no retry when commands wait");
}

}  // namespace

int main()
{
    testClock();
    testConnectFailure();
    testTimeouts();
    testReplyClass();
    testStartingAfterHello();
    testLinkState();
    testLatencySensitiveKeepsOnline();
    testShouldSendHello();
    std::cout << "link_logic_test: ok\n";
    return 0;
}
