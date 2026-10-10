/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の net/frame_stats (frame の形式の選択、符号化失敗の注入、logStats の集計。
// docs/design/step6-cloud-device.md §3.3, §3.4) をホストで確かめる。
#include <apps/app_photobooth/config.h>
#include <apps/app_photobooth/net/frame_stats.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

namespace frame  = photobooth::net::frame;
namespace config = photobooth::config;

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

void expectNear(float actual, float expected, const char* label)
{
    if (std::fabs(actual - expected) > 1e-3f) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void testFormat()
{
    expectTrue(frame::selectFormat(true) == frame::Format::Jpeg, "jpeg enabled -> Jpeg");
    expectTrue(frame::selectFormat(false) == frame::Format::Rgb565, "jpeg disabled -> Rgb565");
    expectTrue(std::strcmp(frame::formatHeader(frame::Format::Jpeg), "jpeg") == 0, "X-Format jpeg");
    expectTrue(std::strcmp(frame::formatHeader(frame::Format::Rgb565), "rgb565") == 0, "X-Format rgb565");
}

void testConfigDefaults()
{
    // テストは PHOTOBOOTH_FRAME_RGB565 / PHOTOBOOTH_JPEG_FAIL_EVERY を定義しないので、既定値が見える。
    expectTrue(config::FRAME_FORMAT_JPEG, "default is JPEG");
    expectEqual(config::FRAME_JPEG_QUALITY, 80, "quality 80");
    expectEqual(config::JPEG_FAIL_EVERY, 0, "no failure injection by default");
}

void testInjectEncodeFailure()
{
    for (uint32_t n = 0; n < 10; ++n) {
        expectTrue(!frame::injectEncodeFailure(n, 0), "every=0 never fails");
    }
    expectTrue(!frame::injectEncodeFailure(1, 3), "n=1 every=3");
    expectTrue(!frame::injectEncodeFailure(2, 3), "n=2 every=3");
    expectTrue(frame::injectEncodeFailure(3, 3), "n=3 every=3");
    expectTrue(frame::injectEncodeFailure(6, 3), "n=6 every=3");
    expectTrue(!frame::injectEncodeFailure(0, 3), "n=0 is not a real attempt");
    expectTrue(frame::injectEncodeFailure(1, 1), "every=1 always fails");
    // 通し番号が一周しても割り切れるときだけ失敗する
    expectTrue(!frame::injectEncodeFailure(UINT32_MAX, 2), "UINT32_MAX is odd");
}

void testEmptyWindow()
{
    frame::Stats st;
    expectTrue(st.empty(), "new window is empty");
    const frame::Summary s = st.summarize(5000);
    expectEqual(s.sent, 0, "empty sent");
    expectEqual(s.attempts, 0, "empty attempts");
    expectNear(s.fps, 0.0f, "empty fps");
    expectEqual(s.rtt_avg_ms, 0, "empty rtt avg (no division by zero)");
    expectEqual(s.encode_avg_ms, 0, "empty encode avg");
    expectEqual(s.jpeg_avg_bytes, 0, "empty jpeg avg");
}

void testSummary()
{
    frame::Stats st;
    // 3 枚符号化 (うち 1 枚は送信失敗)、1 枚は符号化失敗、1 枚は offline で捨てた
    st.addEncoded(20, 15000);
    st.addSent(100, 30);
    st.addEncoded(40, 16000);
    st.addSent(200, 50);
    st.addEncoded(30, 20000);
    st.addSendFailure();
    st.addEncodeFailure();
    st.addSkipped();
    expectTrue(!st.empty(), "window has events");

    const frame::Summary s = st.summarize(4000);
    expectEqual(s.sent, 2, "sent");
    expectEqual(s.send_failures, 1, "send failures");
    expectEqual(s.encode_failures, 1, "encode failures");
    expectEqual(s.skipped, 1, "skipped");
    expectEqual(s.attempts, 4, "attempts = sent + send failures + encode failures (skipped excluded)");
    expectNear(s.fps, 0.5f, "fps = sent / dt");
    expectEqual(s.rtt_avg_ms, 150, "rtt avg");
    expectEqual(s.rtt_max_ms, 200, "rtt max");
    expectEqual(s.edge_avg_ms, 40, "edge avg");
    expectEqual(s.encoded, 3, "encoded");
    expectEqual(s.encode_avg_ms, 30, "encode avg");
    expectEqual(s.encode_max_ms, 40, "encode max");
    expectEqual(s.jpeg_avg_bytes, 17000, "jpeg avg");
    expectEqual(s.jpeg_max_bytes, 20000, "jpeg max");

    expectNear(st.summarize(0).fps, 0.0f, "dt=0 gives fps 0");

    st.reset();
    expectTrue(st.empty(), "reset empties the window");
    expectEqual(st.summarize(5000).jpeg_max_bytes, 0, "reset clears max");
}

void testEncodeFailureOnlyWindowIsLogged()
{
    // 符号化に失敗し続けて 1 枚も送れない窓でもログを出す (試験 22)
    frame::Stats st;
    st.addEncodeFailure();
    expectTrue(!st.empty(), "encode failure alone is reported");
    const frame::Summary s = st.summarize(5000);
    expectEqual(s.attempts, 1, "attempts counts the discarded frame");
    expectEqual(s.encode_failures, 1, "encode failure counted");
    expectEqual(s.encoded, 0, "nothing encoded");
}

void testLargeSums()
{
    // 平均の和が 32 ビットを超えても壊れない (180 KiB 近い JPEG が続いた場合)
    frame::Stats st;
    for (int i = 0; i < 30000; ++i) {
        st.addEncoded(10, 180000);
    }
    const frame::Summary s = st.summarize(5000);
    expectEqual(s.jpeg_avg_bytes, 180000, "large jpeg avg");
    expectEqual(s.jpeg_max_bytes, 180000, "large jpeg max");
}

}  // namespace

int main()
{
    testFormat();
    testConfigDefaults();
    testInjectEncodeFailure();
    testEmptyWindow();
    testSummary();
    testEncodeFailureOnlyWindowIsLogged();
    testLargeSums();
    std::cout << "frame_stats_test: OK\n";
    return 0;
}
