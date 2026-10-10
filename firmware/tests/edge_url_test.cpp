/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の net/edge_url (EDGE_BASE_URL の分解。docs/design/step6-cloud-device.md §3.2, 試験 12) をホストで確かめる。
#include <apps/app_photobooth/net/edge_url.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using photobooth::net::EdgeUrl;
using photobooth::net::formatEdgeUrl;
using photobooth::net::parseEdgeUrl;
using photobooth::net::UrlStatus;
using photobooth::net::urlStatusName;

void expectEqual(long long actual, long long expected, const std::string& label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void expectStr(const char* actual, const char* expected, const std::string& label)
{
    if (std::strcmp(actual, expected) != 0) {
        std::cerr << label << ": expected \"" << expected << "\", got \"" << actual << "\"\n";
        std::exit(1);
    }
}

void expectOk(const char* url, bool https, const char* host, unsigned port)
{
    EdgeUrl u;
    const UrlStatus s = parseEdgeUrl(url, u);
    const std::string label = std::string("ok: ") + url;
    if (s != UrlStatus::Ok) {
        std::cerr << label << ": expected ok, got " << urlStatusName(s) << '\n';
        std::exit(1);
    }
    expectEqual(u.https ? 1 : 0, https ? 1 : 0, label + " https");
    expectStr(u.host, host, label + " host");
    expectEqual(u.port, port, label + " port");
}

void expectBad(const char* url, UrlStatus expected)
{
    EdgeUrl u;
    const UrlStatus s = parseEdgeUrl(url, u);
    if (s != expected) {
        std::cerr << "bad: " << (url ? url : "(null)") << ": expected " << urlStatusName(expected) << ", got "
                  << urlStatusName(s) << '\n';
        std::exit(1);
    }
}

void testValid()
{
    // 既定 port
    expectOk("http://192.168.0.167", false, "192.168.0.167", 80);
    expectOk("https://stackchan-edge.example.workers.dev", true, "stackchan-edge.example.workers.dev", 443);
    // 明示 port
    expectOk("http://192.168.0.167:8765", false, "192.168.0.167", 8765);
    expectOk("https://abc-def.trycloudflare.com:8443", true, "abc-def.trycloudflare.com", 8443);
    expectOk("http://localhost:1", false, "localhost", 1);
    expectOk("http://edge:65535", false, "edge", 65535);
    // 末尾の "/" は 1 つだけ許して捨てる
    expectOk("http://192.168.0.167:8765/", false, "192.168.0.167", 8765);
    expectOk("https://x.example/", true, "x.example", 443);
    // scheme の大小文字は区別しない (小文字に正規化)
    expectOk("HTTPS://x.example", true, "x.example", 443);
    expectOk("Http://10.0.0.1:80", false, "10.0.0.1", 80);
    // host の大小文字はそのまま
    expectOk("https://Edge.Example.COM", true, "Edge.Example.COM", 443);
    // host ちょうど 64 文字
    const std::string h64 = std::string(60, 'a') + ".com";
    expectOk(("https://" + h64).c_str(), true, h64.c_str(), 443);
}

void testInvalid()
{
    expectBad(nullptr, UrlStatus::BadScheme);
    expectBad("", UrlStatus::BadScheme);
    expectBad("192.168.0.167:8765", UrlStatus::BadScheme);
    expectBad("ftp://x.example", UrlStatus::BadScheme);
    expectBad("http:/x.example", UrlStatus::BadScheme);
    expectBad("httpss://x.example", UrlStatus::BadScheme);
    expectBad(" http://x.example", UrlStatus::BadScheme);
    // path / query / fragment
    expectBad("http://x.example/v1", UrlStatus::PathNotAllowed);
    expectBad("http://x.example//", UrlStatus::PathNotAllowed);
    expectBad("http://x.example:8765/api/", UrlStatus::PathNotAllowed);
    expectBad("http://x.example?a=1", UrlStatus::PathNotAllowed);
    expectBad("http://x.example/?a=1", UrlStatus::PathNotAllowed);
    expectBad("http://x.example#top", UrlStatus::PathNotAllowed);
    // userinfo
    expectBad("http://user:pass@x.example", UrlStatus::Userinfo);
    expectBad("https://user@x.example", UrlStatus::Userinfo);
    // IPv6
    expectBad("http://[::1]:8765", UrlStatus::Ipv6);
    expectBad("https://[2001:db8::1]", UrlStatus::Ipv6);
    // 長い host (65 文字)
    const std::string h65 = std::string(61, 'a') + ".com";
    expectBad(("https://" + h65).c_str(), UrlStatus::HostTooLong);
    // host の形
    expectBad("http://", UrlStatus::BadHost);
    expectBad("http://:8765", UrlStatus::BadHost);
    expectBad("http://x..example", UrlStatus::BadHost);
    expectBad("http://.example", UrlStatus::BadHost);
    expectBad("http://x.example.", UrlStatus::BadHost);
    expectBad("http://-x.example", UrlStatus::BadHost);
    expectBad("http://x-.example", UrlStatus::BadHost);
    expectBad("http://x_y.example", UrlStatus::BadHost);
    expectBad("http://x.example ", UrlStatus::BadHost);
    expectBad("http://256.0.0.1", UrlStatus::BadHost);
    expectBad("http://1.2.3", UrlStatus::BadHost);
    expectBad("http://1.2.3.4.5", UrlStatus::BadHost);
    expectBad("http://1.2.3.", UrlStatus::BadHost);
    expectBad("http://1234.1.1.1", UrlStatus::BadHost);
    expectBad(("http://" + std::string(64, 'a')).c_str(), UrlStatus::BadHost);  // ラベル 64 文字
    // port 範囲外・不正
    expectBad("http://x.example:0", UrlStatus::BadPort);
    expectBad("http://x.example:65536", UrlStatus::BadPort);
    expectBad("http://x.example:99999", UrlStatus::BadPort);
    expectBad("http://x.example:123456", UrlStatus::BadPort);
    expectBad("http://x.example:", UrlStatus::BadPort);
    expectBad("http://x.example:80a", UrlStatus::BadPort);
    expectBad("http://x.example:-1", UrlStatus::BadPort);
    expectBad("http://x.example:80:81", UrlStatus::BadPort);
    // https + IP は不可 (http + IP は可)
    expectBad("https://192.168.0.167", UrlStatus::HttpsWithIp);
    expectBad("https://192.168.0.167:8443/", UrlStatus::HttpsWithIp);
}

void testOutputClearedOnError()
{
    EdgeUrl u;
    parseEdgeUrl("https://x.example:8443", u);
    parseEdgeUrl("https://x.example/path", u);
    expectStr(u.host, "", "host cleared on error");
    expectEqual(u.port, 0, "port cleared on error");
}

void testFormat()
{
    EdgeUrl u;
    parseEdgeUrl("HTTPS://x.example/", u);
    char buf[96];
    expectEqual(static_cast<long long>(formatEdgeUrl(u, buf, sizeof(buf))), 21, "format length");
    expectStr(buf, "https://x.example:443", "format https default port");
    parseEdgeUrl("http://192.168.0.167:8765", u);
    formatEdgeUrl(u, buf, sizeof(buf));
    expectStr(buf, "http://192.168.0.167:8765", "format http");
    char small[8];
    expectEqual(static_cast<long long>(formatEdgeUrl(u, small, sizeof(small))), 7, "format truncated length");
    expectStr(small, "http://", "format truncated");
    // 最長 (https + 64 文字 + 5 桁 port) でも診断の 96 バイトに入る
    const std::string h64 = std::string(60, 'a') + ".com";
    parseEdgeUrl(("https://" + h64 + ":65535").c_str(), u);
    expectEqual(static_cast<long long>(formatEdgeUrl(u, buf, sizeof(buf))), 8 + 64 + 6, "format longest");
}

}  // namespace

int main()
{
    testValid();
    testInvalid();
    testOutputClearedOnError();
    testFormat();
    std::cout << "edge_url_test: ok\n";
    return 0;
}
