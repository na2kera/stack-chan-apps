/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "edge_url.h"

#include <cstdio>
#include <cstring>

namespace photobooth::net {

namespace {

char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isAlnum(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// s の先頭が scheme "://" なら scheme の長さを含めて読み進め、https かどうかを返す。
bool matchScheme(const char*& p, bool& https)
{
    static const char* const kSchemes[] = {"https", "http"};  // 長い方から試す
    for (const char* scheme : kSchemes) {
        const size_t n = strlen(scheme);
        bool same      = true;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\0' || lower(p[i]) != scheme[i]) {
                same = false;
                break;
            }
        }
        if (same && strncmp(p + n, "://", 3) == 0) {
            https = n == 5;
            p += n + 3;
            return true;
        }
    }
    return false;
}

// 数字と '.' だけからなる host を IPv4 の 4 つ組として確かめる。
bool validIpv4(const char* h, size_t len)
{
    int parts  = 0;
    size_t i   = 0;
    while (i <= len) {
        size_t digits = 0;
        unsigned v    = 0;
        while (i < len && isDigit(h[i])) {
            v = v * 10 + static_cast<unsigned>(h[i] - '0');
            ++digits;
            ++i;
            if (digits > 3) return false;
        }
        if (digits == 0 || v > 255) return false;
        ++parts;
        if (i == len) break;
        if (h[i] != '.') return false;
        ++i;
    }
    return parts == 4;
}

// DNS 名: ラベル (1〜63 文字、英数字と '-'、先頭と末尾は '-' 以外) を '.' で区切ったもの。
bool validDnsName(const char* h, size_t len)
{
    size_t label = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i == len || h[i] == '.') {
            if (label == 0 || label > 63) return false;
            if (h[i - 1] == '-' || h[i - label] == '-') return false;
            label = 0;
            continue;
        }
        if (!isAlnum(h[i]) && h[i] != '-') return false;
        ++label;
    }
    return true;
}

}  // namespace

UrlStatus parseEdgeUrl(const char* url, EdgeUrl& out)
{
    out = EdgeUrl{};
    if (url == nullptr) {
        return UrlStatus::BadScheme;
    }
    const char* p = url;
    bool https    = false;
    if (!matchScheme(p, https)) {
        return UrlStatus::BadScheme;
    }

    // authority の終わり ('/', '?', '#', 終端) までを見る。
    const char* auth = p;
    size_t auth_len  = strcspn(auth, "/?#");
    if (memchr(auth, '@', auth_len) != nullptr) {
        return UrlStatus::Userinfo;
    }
    if (auth_len > 0 && auth[0] == '[') {
        return UrlStatus::Ipv6;
    }
    const char* colon = static_cast<const char*>(memchr(auth, ':', auth_len));
    const size_t host_len = colon != nullptr ? static_cast<size_t>(colon - auth) : auth_len;
    if (host_len == 0) {
        return UrlStatus::BadHost;
    }
    if (host_len > kEdgeHostMax) {
        return UrlStatus::HostTooLong;
    }

    bool numeric = true;
    for (size_t i = 0; i < host_len; ++i) {
        if (!isDigit(auth[i]) && auth[i] != '.') {
            numeric = false;
            break;
        }
    }
    if (numeric ? !validIpv4(auth, host_len) : !validDnsName(auth, host_len)) {
        return UrlStatus::BadHost;
    }

    uint32_t port = https ? 443 : 80;
    if (colon != nullptr) {
        const char* ps    = colon + 1;
        const size_t plen = auth_len - host_len - 1;
        if (plen == 0 || plen > 5) {
            return UrlStatus::BadPort;
        }
        port = 0;
        for (size_t i = 0; i < plen; ++i) {
            if (!isDigit(ps[i])) {
                return UrlStatus::BadPort;
            }
            port = port * 10 + static_cast<uint32_t>(ps[i] - '0');
        }
        if (port == 0 || port > 65535) {
            return UrlStatus::BadPort;
        }
    }

    // authority の後ろ: 何も無いか、"/" 1 つだけ。
    const char* rest = auth + auth_len;
    if (!(rest[0] == '\0' || (rest[0] == '/' && rest[1] == '\0'))) {
        return UrlStatus::PathNotAllowed;
    }
    if (https && numeric) {
        return UrlStatus::HttpsWithIp;
    }

    out.https = https;
    memcpy(out.host, auth, host_len);
    out.host[host_len] = '\0';
    out.port           = static_cast<uint16_t>(port);
    return UrlStatus::Ok;
}

size_t formatEdgeUrl(const EdgeUrl& url, char* buf, size_t n)
{
    if (buf == nullptr || n == 0) {
        return 0;
    }
    const int r = snprintf(buf, n, "%s://%s:%u", url.https ? "https" : "http", url.host,
                           static_cast<unsigned>(url.port));
    if (r < 0) {
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(r) < n ? static_cast<size_t>(r) : n - 1;
}

const char* urlStatusName(UrlStatus s)
{
    switch (s) {
        case UrlStatus::Ok:
            return "ok";
        case UrlStatus::BadScheme:
            return "bad_scheme";
        case UrlStatus::Userinfo:
            return "userinfo";
        case UrlStatus::Ipv6:
            return "ipv6";
        case UrlStatus::BadHost:
            return "bad_host";
        case UrlStatus::HostTooLong:
            return "host_too_long";
        case UrlStatus::BadPort:
            return "bad_port";
        case UrlStatus::PathNotAllowed:
            return "path_not_allowed";
        case UrlStatus::HttpsWithIp:
            return "https_with_ip";
    }
    return "?";
}

}  // namespace photobooth::net
