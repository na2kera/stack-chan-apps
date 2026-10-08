/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の flow/time_format (写真の期限の表示。docs/design/testable-logic-step1.md §5 F) をホストで確かめる。
#include <apps/app_photobooth/flow/time_format.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

using photobooth::flow::formatExpires;

void fail(const char* label)
{
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
}

void expectStr(const char* actual, const char* expected, const char* label)
{
    if (std::strcmp(actual, expected) != 0) {
        std::cerr << label << ": expected \"" << expected << "\", got \"" << actual << "\"\n";
        std::exit(1);
    }
}

void expectTrue(bool value, const char* label)
{
    if (!value) {
        fail(label);
    }
}

// formatExpires(iso) の結果 (十分な大きさの out)。
const char* format(const char* iso)
{
    static char out[16];
    std::memset(out, 'x', sizeof(out));
    formatExpires(iso, out, sizeof(out));
    return out;
}

void testOk()
{
    expectStr(format("2026-09-30T22:00:00+09:00"), "22:00", "JST");
    expectStr(format("2026-01-01T00:05:59Z"), "00:05", "UTC with Z");
    // 16 文字ちょうど (秒なし) でも読める
    expectStr(format("2026-09-30T07:45"), "07:45", "exactly 16 chars");
}

void testUnknown()
{
    expectStr(format(nullptr), "--:--", "nullptr");
    expectStr(format(""), "--:--", "empty");
    expectStr(format("2026-09-30T22:0"), "--:--", "15 chars (too short)");
    expectStr(format("2026-09-30 22:00:00+09:00"), "--:--", "space instead of T");
    expectStr(format("2026-09-30T2200:00+09:00"), "--:--", "colon at wrong position");
    expectStr(format("26-09-30T22:00:00+09:00"), "--:--", "short year shifts T");
}

void testTruncate()
{
    // len が小さいときは NUL を含めて len バイトに切り詰める
    char out[4];
    formatExpires("2026-09-30T22:00:00+09:00", out, sizeof(out));
    expectStr(out, "22:", "truncated to len 4");
    formatExpires(nullptr, out, sizeof(out));
    expectStr(out, "--:", "unknown truncated to len 4");

    char one[1] = {'x'};
    formatExpires("2026-09-30T22:00:00+09:00", one, sizeof(one));
    expectTrue(one[0] == '\0', "len 1 gives empty string");

    // len 0 なら何も書かない
    char zero[2] = {'a', 'b'};
    formatExpires("2026-09-30T22:00:00+09:00", zero, 0);
    expectTrue(zero[0] == 'a' && zero[1] == 'b', "len 0 writes nothing");
}

}  // namespace

int main()
{
    testOk();
    testUnknown();
    testTruncate();
    std::cout << "time_format_test: ok\n";
    return 0;
}
