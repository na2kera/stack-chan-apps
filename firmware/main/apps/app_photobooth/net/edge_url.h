/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge の接続先 URL (config_local.h の EDGE_BASE_URL) を scheme / host / port に分ける純粋関数
// (docs/design/step6-cloud-device.md §3.2「URL の文法」)。ロジック層: ESP-IDF・FreeRTOS のヘッダを含めない。
//
// 文法: scheme "://" host [":" port] ["/"]
//   - scheme は http / https (大小文字を区別しない。小文字に正規化する)。
//   - host は DNS 名か IPv4 (1〜64 文字)。IPv6 ("[...]") と userinfo ("user@") は不可。
//   - port は 1〜65535。省略時は http 80 / https 443。
//   - 末尾の "/" は 1 つだけ許して捨てる。path・query・fragment があれば不正。
//   - https の接続先は IP では書けない (証明書の名前の検査に host 名が要る)。
// path を持たないのは、リクエストごとに path を差し替える既存の構造 (host / port / path を別に持つ) のため。
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::net {

inline constexpr size_t kEdgeHostMax = 64;  // host の最大文字数

struct EdgeUrl {
    bool https                 = false;
    char host[kEdgeHostMax + 1] = {};
    uint16_t port              = 0;
};

enum class UrlStatus : uint8_t {
    Ok,
    BadScheme,       // http / https 以外、または "://" が無い
    Userinfo,        // "user@host"
    Ipv6,            // "[::1]"
    BadHost,         // 空・使えない文字・ラベルの形が不正・IPv4 の形が不正
    HostTooLong,     // 64 文字を超える
    BadPort,         // 空・数字以外・0・65535 超
    PathNotAllowed,  // 末尾の "/" 1 つ以外の path、query、fragment
    HttpsWithIp,     // https で IPv4 を指定した
};

// url を out に分解する。Ok 以外なら out の中身は使わない。
UrlStatus parseEdgeUrl(const char* url, EdgeUrl& out);

// 診断画面用の "scheme://host:port" (port は省略していても書く。path と鍵は含まない)。
// 書いた文字数 (NUL を除く) を返す。n が足りなければ切り詰める。
size_t formatEdgeUrl(const EdgeUrl& url, char* buf, size_t n);

// ログ用の短い名前 (例 "bad_port")。URL 自体は含まない。
const char* urlStatusName(UrlStatus s);

}  // namespace photobooth::net
