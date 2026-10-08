/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_roulette の hw/light_pattern (頭部 LED の色の計算。docs/design/testable-logic-step1.md §5 E) をホストで確かめる。
#include <apps/app_roulette/hw/light_pattern.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {

using roulette::hw::hueToRgb;
using roulette::hw::kMaxLevel;
using roulette::hw::LightPattern;
using roulette::hw::Rgb;

constexpr int kLeds = LightPattern::kLedCount;

void fail(const char* label)
{
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
}

void expectEqual(int actual, int expected, const char* label)
{
    if (actual != expected) {
        std::cerr << label << ": expected " << expected << ", got " << actual << '\n';
        std::exit(1);
    }
}

void expectTrue(bool value, const char* label)
{
    if (!value) {
        fail(label);
    }
}

void expectRgb(const Rgb& c, int r, int g, int b, const char* label)
{
    if (c.r != r || c.g != g || c.b != b) {
        std::cerr << label << ": expected (" << r << "," << g << "," << b << "), got (" << int(c.r) << ","
                  << int(c.g) << "," << int(c.b) << ")\n";
        std::exit(1);
    }
}

void expectHue(uint32_t hue, int r, int g, int b, const char* label)
{
    Rgb c;
    hueToRgb(hue, c.r, c.g, c.b);
    expectRgb(c, r, g, b, label);
}

void expectAll(const Rgb (&out)[kLeds], int r, int g, int b, const char* label)
{
    for (const auto& c : out) {
        expectRgb(c, r, g, b, label);
    }
}

bool same(const Rgb (&a)[kLeds], const Rgb (&b)[kLeds])
{
    for (int i = 0; i < kLeds; ++i) {
        if (a[i].r != b[i].r || a[i].g != b[i].g || a[i].b != b[i].b) {
            return false;
        }
    }
    return true;
}

void testHueToRgb()
{
    // 6 区間の端
    expectHue(0, 24, 0, 0, "hue 0 (red)");
    expectHue(59, 24, 23, 0, "hue 59");
    expectHue(60, 24, 24, 0, "hue 60 (yellow)");
    expectHue(120, 0, 24, 0, "hue 120 (green)");
    expectHue(180, 0, 24, 24, "hue 180 (cyan)");
    expectHue(240, 0, 0, 24, "hue 240 (blue)");
    expectHue(300, 24, 0, 24, "hue 300 (magenta)");
    expectHue(359, 24, 0, 1, "hue 359");
    expectHue(30, 24, 12, 0, "hue 30");
    // 360 以上は余り
    expectHue(360, 24, 0, 0, "hue 360 = 0");
    expectHue(360 + 120, 0, 24, 0, "hue 480 = 120");

    // 最大成分が kMaxLevel を超えない (どの色相でも)
    for (uint32_t h = 0; h < 720; ++h) {
        Rgb c;
        hueToRgb(h, c.r, c.g, c.b);
        expectTrue(c.r <= kMaxLevel && c.g <= kMaxLevel && c.b <= kMaxLevel, "component <= kMaxLevel");
        expectTrue(c.r == kMaxLevel || c.g == kMaxLevel || c.b == kMaxLevel, "one component at kMaxLevel");
    }
}

void testInitialOff()
{
    expectEqual(kLeds, 12, "12 LEDs (0-5 left, 6-11 right)");
    LightPattern p;
    Rgb out[kLeds];
    expectTrue(p.colors(0, out), "first colors() is a change");
    expectAll(out, 0, 0, 0, "initially off");
    expectTrue(!p.colors(1000, out), "off stays unchanged");
}

void testRainbow()
{
    LightPattern p;
    Rgb out[kLeds];
    const uint32_t t0 = 5000;
    p.startRainbow(t0);
    expectTrue(p.colors(t0, out), "rainbow start is a change");
    // LED i は色相 i * 30 から
    expectRgb(out[0], 24, 0, 0, "led 0 at start");
    expectRgb(out[1], 24, 12, 0, "led 1 at start");
    expectRgb(out[4], 0, 24, 0, "led 4 at start (hue 120)");
    expectRgb(out[8], 0, 0, 24, "led 8 at start (hue 240)");

    // 50 ms 未満は再計算しない
    Rgb first[kLeds];
    for (int i = 0; i < kLeds; ++i) first[i] = out[i];
    expectTrue(!p.colors(t0 + 49, out), "no redraw before 50 ms");
    expectTrue(same(out, first), "same colors before 50 ms");
    // 50 ms で色相が 22° 進む (50 * 360 / 800)
    expectTrue(p.colors(t0 + 50, out), "redraw at 50 ms");
    expectRgb(out[0], 24, 8, 0, "led 0 at 50 ms (hue 22)");
    // 次の更新は前回描いた時刻から 50 ms
    expectTrue(!p.colors(t0 + 99, out), "no redraw 49 ms after last draw");
    expectTrue(p.colors(t0 + 100, out), "redraw 50 ms after last draw");
    expectRgb(out[0], 24, 18, 0, "led 0 at 100 ms (hue 45)");

    // 1200 ms で消える (1199 ms はまだ虹色)
    p.colors(t0 + 1199, out);
    bool any_lit = false;
    for (const auto& c : out) any_lit = any_lit || c.r || c.g || c.b;
    expectTrue(any_lit, "still rainbow at 1199 ms");
    expectTrue(p.colors(t0 + 1200, out), "off at 1200 ms is a change");
    expectAll(out, 0, 0, 0, "off at 1200 ms");
    expectTrue(!p.colors(t0 + 5000, out), "stays off");
}

void testRainbowLateUpdate()
{
    // 1.2 秒より後に初めて colors() を呼んでも消灯する。
    LightPattern p;
    Rgb out[kLeds];
    p.startRainbow(0);
    expectTrue(p.colors(3000, out), "late colors() is a change");
    expectAll(out, 0, 0, 0, "late colors() is off");
}

void testReach()
{
    LightPattern p;
    Rgb out[kLeds];
    p.startRainbow(1000);
    p.colors(1000, out);
    // 虹色の途中でも打ち切って黄色
    p.setReach(true);
    expectTrue(p.colors(1100, out), "reach is a change");
    expectAll(out, 24, 18, 0, "reach yellow");
    // 虹色の 1.2 秒のタイマーで消さない
    expectTrue(!p.colors(1000 + 5000, out), "reach stays");
    expectAll(out, 24, 18, 0, "reach not cleared by rainbow timer");
    p.setReach(true);
    expectTrue(!p.colors(7000, out), "reach again is not a change");

    p.setReach(false);
    expectTrue(p.colors(7000, out), "reach off is a change");
    expectAll(out, 0, 0, 0, "reach off");

    p.setReach(true);
    p.colors(8000, out);
    p.off();
    expectTrue(p.colors(8000, out), "off() is a change");
    expectAll(out, 0, 0, 0, "off()");

    // off() で虹色も止まる
    p.startRainbow(9000);
    p.colors(9000, out);
    p.off();
    expectTrue(p.colors(9050, out), "off during rainbow is a change");
    expectAll(out, 0, 0, 0, "off during rainbow");
    expectTrue(!p.colors(9100, out), "no rainbow after off");
}

void testRestartRainbow()
{
    // 2 回目の startRainbow で時刻を取り直す。
    LightPattern p;
    Rgb out[kLeds];
    p.startRainbow(0);
    p.colors(0, out);
    p.startRainbow(1000);
    p.colors(1000, out);
    expectRgb(out[0], 24, 0, 0, "restart begins at hue 0");
    p.colors(2199, out);
    bool any_lit = false;
    for (const auto& c : out) any_lit = any_lit || c.r || c.g || c.b;
    expectTrue(any_lit, "restarted rainbow lasts 1.2 s from restart");
}

void testMillisWrap()
{
    LightPattern p;
    Rgb out[kLeds];
    const uint32_t t0 = 0xFFFFFFFFu - 100;
    p.startRainbow(t0);
    p.colors(t0, out);
    // 一周した直後 (経過 101 ms) はまだ虹色で、50 ms ごとの更新も効く
    expectTrue(p.colors(t0 + 101, out), "redraw across wrap");
    bool any_lit = false;
    for (const auto& c : out) any_lit = any_lit || c.r || c.g || c.b;
    expectTrue(any_lit, "rainbow across wrap");
    expectTrue(!p.colors(t0 + 120, out), "no redraw within 50 ms across wrap");
    // 経過 1200 ms で消える
    expectTrue(p.colors(t0 + 1199, out), "still rainbow before 1200 ms across wrap");
    expectTrue(p.colors(t0 + 1200, out), "off at 1200 ms across wrap");
    expectAll(out, 0, 0, 0, "off across wrap");
}

}  // namespace

int main()
{
    testHueToRgb();
    testInitialOff();
    testRainbow();
    testRainbowLateUpdate();
    testReach();
    testRestartRainbow();
    testMillisWrap();
    std::cout << "light_pattern_test: ok\n";
    return 0;
}
