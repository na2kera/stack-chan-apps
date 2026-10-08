/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の view/jpeg_info (JPEG のヘッダ解析。docs/design/testable-logic-step1.md §3 B) をホストで確かめる。
#include <apps/app_photobooth/view/jpeg_info.h>

#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace {

using photobooth::view::JpegInfo;
using photobooth::view::JpegStatus;
using photobooth::view::readJpegInfo;

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

void expectStatus(JpegStatus actual, JpegStatus expected, const char* label)
{
    expectEqual(static_cast<int>(actual), static_cast<int>(expected), label);
}

// JPEG のヘッダ部分のバイト列を組み立てる。
class JpegBuilder {
public:
    JpegBuilder()
    {
        bytes_ = {0xFF, 0xD8};  // SOI
    }

    // 長さを持つセグメント (長さ欄は body + 2 で自動)。
    JpegBuilder& segment(uint8_t marker, const std::vector<uint8_t>& body)
    {
        bytes_.push_back(0xFF);
        bytes_.push_back(marker);
        const size_t seg = body.size() + 2;
        bytes_.push_back(static_cast<uint8_t>(seg >> 8));
        bytes_.push_back(static_cast<uint8_t>(seg & 0xFF));
        bytes_.insert(bytes_.end(), body.begin(), body.end());
        return *this;
    }

    // SOF (精度・高さ・幅・成分数と、成分ごとの 3 バイト)。
    JpegBuilder& sof(uint8_t marker, int precision, int width, int height, int components)
    {
        std::vector<uint8_t> body = {
            static_cast<uint8_t>(precision),  static_cast<uint8_t>(height >> 8), static_cast<uint8_t>(height & 0xFF),
            static_cast<uint8_t>(width >> 8), static_cast<uint8_t>(width & 0xFF), static_cast<uint8_t>(components),
        };
        for (int i = 0; i < components; ++i) {
            body.insert(body.end(), {static_cast<uint8_t>(i + 1), 0x11, 0x00});
        }
        return segment(marker, body);
    }

    // APP0 (JFIF)。
    JpegBuilder& app0()
    {
        return segment(0xE0, {'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0});
    }

    // DQT (8-bit の表 1 つ)。
    JpegBuilder& dqt()
    {
        std::vector<uint8_t> body(65, 1);
        body[0] = 0;
        return segment(0xDB, body);
    }

    JpegBuilder& raw(std::initializer_list<uint8_t> bytes)
    {
        bytes_.insert(bytes_.end(), bytes);
        return *this;
    }

    const std::vector<uint8_t>& bytes() const
    {
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
};

JpegStatus run(const std::vector<uint8_t>& bytes, JpegInfo& out)
{
    return readJpegInfo(bytes.data(), bytes.size(), out);
}

void testBaseline()
{
    JpegBuilder b;
    b.sof(0xC0, 8, 320, 240, 3);
    JpegInfo out;
    expectStatus(run(b.bytes(), out), JpegStatus::Ok, "SOF0 status");
    expectEqual(out.width, 320, "SOF0 width");
    expectEqual(out.height, 240, "SOF0 height");
    expectEqual(out.components, 3, "SOF0 components");
    expectEqual(out.precision, 8, "SOF0 precision");
    expectEqual(out.sof, 0xC0, "SOF0 marker");

    JpegBuilder gray;
    gray.sof(0xC0, 8, 1, 1, 1);
    expectStatus(run(gray.bytes(), out), JpegStatus::Ok, "SOF0 grayscale (1 component)");
    expectTrue(out.width == 1 && out.height == 1, "SOF0 grayscale 1x1");
}

void testSkipsSegments()
{
    // APP0 (JFIF)・DQT・DHT (0xC4 は SOF ではない) を挟む。
    JpegBuilder b;
    b.app0().dqt().segment(0xC4, std::vector<uint8_t>(20, 0)).sof(0xC0, 8, 640, 480, 3);
    JpegInfo out;
    expectStatus(run(b.bytes(), out), JpegStatus::Ok, "APP0 + DQT + DHT then SOF0");
    expectEqual(out.width, 640, "after segments width");
    expectEqual(out.height, 480, "after segments height");
}

void testFillBytes()
{
    // マーカーの前の詰め物の 0xFF が続いてもよい。
    JpegBuilder b;
    b.raw({0xFF, 0xFF, 0xFF}).sof(0xC0, 8, 16, 8, 3);
    JpegInfo out;
    expectStatus(run(b.bytes(), out), JpegStatus::Ok, "fill 0xFF before marker");
    expectEqual(out.width, 16, "fill width");
}

void testSkipsMarkersWithoutLength()
{
    // RSTn (0xD0..0xD7) と TEM (0x01) は長さを持たない。
    JpegBuilder b;
    b.raw({0xFF, 0xD0, 0xFF, 0xD7, 0xFF, 0x01}).sof(0xC0, 8, 32, 24, 3);
    JpegInfo out;
    expectStatus(run(b.bytes(), out), JpegStatus::Ok, "RSTn / TEM skipped");
    expectEqual(out.height, 24, "RSTn / TEM height");
}

void testUnsupported()
{
    JpegInfo out;
    {
        JpegBuilder b;
        b.sof(0xC2, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Unsupported, "SOF2 (progressive)");
        expectEqual(out.sof, 0xC2, "SOF2 marker is filled");
        expectEqual(out.width, 320, "SOF2 width is filled");
        expectEqual(out.height, 240, "SOF2 height is filled");
    }
    {
        JpegBuilder b;
        b.sof(0xC1, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Unsupported, "SOF1 (extended)");
    }
    {
        JpegBuilder b;
        b.sof(0xC0, 12, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Unsupported, "12-bit");
        expectEqual(out.precision, 12, "12-bit precision is filled");
    }
    {
        JpegBuilder b;
        b.sof(0xC0, 8, 320, 240, 4);
        expectStatus(run(b.bytes(), out), JpegStatus::Unsupported, "4 components");
        expectEqual(out.components, 4, "4 components is filled");
    }
}

void testInvalid()
{
    JpegInfo out;
    expectStatus(readJpegInfo(nullptr, 100, out), JpegStatus::Invalid, "nullptr");

    JpegBuilder ok;
    ok.sof(0xC0, 8, 320, 240, 3);
    const auto& bytes = ok.bytes();
    expectStatus(readJpegInfo(bytes.data(), 3, out), JpegStatus::Invalid, "shorter than 4 bytes");

    {
        std::vector<uint8_t> v = bytes;
        v[1]                   = 0xD9;
        expectStatus(run(v, out), JpegStatus::Invalid, "not SOI");
    }
    {
        // SOF の途中で切れている (セグメント長がバッファを超える)
        expectStatus(readJpegInfo(bytes.data(), bytes.size() - 1, out), JpegStatus::Invalid,
                     "segment longer than buffer");
    }
    {
        // SOI のあとに何も無い (マーカーの 0xFF だけ)
        JpegBuilder b;
        b.raw({0xFF, 0xFF});
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "only fill bytes");
    }
    {
        // マーカーの位置に 0xFF 以外
        JpegBuilder b;
        b.raw({0x12, 0x34}).sof(0xC0, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "no 0xFF at marker position");
    }
    {
        // SOF の前に SOS
        JpegBuilder b;
        b.segment(0xDA, {1, 1, 0, 0, 0x3F, 0}).sof(0xC0, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "SOS before SOF");
    }
    {
        // SOF の前に EOI
        JpegBuilder b;
        b.raw({0xFF, 0xD9}).sof(0xC0, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "EOI before SOF");
    }
    {
        // 0xFF00 はヘッダに現れない
        JpegBuilder b;
        b.raw({0xFF, 0x00}).sof(0xC0, 8, 320, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "0xFF00 in header");
    }
    {
        JpegBuilder b;
        b.sof(0xC0, 8, 0, 240, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "width 0");
    }
    {
        JpegBuilder b;
        b.sof(0xC0, 8, 320, 0, 3);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "height 0");
    }
    {
        // SOF0 の長さが成分数と合わない (成分数 3 なのに成分の情報が 1 つ分多い)
        JpegBuilder b;
        std::vector<uint8_t> body = {8, 0, 240, 1, 64, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0};
        b.segment(0xC0, body);
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "SOF0 length does not match components");
    }
    {
        // SOF のセグメントが 8 バイト未満
        JpegBuilder b;
        b.segment(0xC0, {8, 0, 240, 1, 64});
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "SOF shorter than 8 bytes");
    }
    {
        // セグメント長が 2 未満
        JpegBuilder b;
        b.raw({0xFF, 0xE0, 0x00, 0x01, 0, 0});
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "segment length < 2");
    }
    {
        // SOF が来ないまま終わる
        JpegBuilder b;
        b.app0();
        expectStatus(run(b.bytes(), out), JpegStatus::Invalid, "no SOF");
    }
}

}  // namespace

int main()
{
    testBaseline();
    testSkipsSegments();
    testFillBytes();
    testSkipsMarkersWithoutLength();
    testUnsupported();
    testInvalid();
    std::cout << "jpeg_info_test: ok\n";
    return 0;
}
