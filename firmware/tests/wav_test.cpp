/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// shared/wav (埋め込み WAV のヘッダ解析。docs/design/testable-logic-step1.md §3 A) をホストで確かめる。
#include <apps/shared/wav.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

using shared::wav::parse;
using shared::wav::Pcm;
using shared::wav::Status;

constexpr uint32_t kRate = 24000;

void fail(const char* label)
{
    std::cerr << "FAILED: " << label << '\n';
    std::exit(1);
}

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
        fail(label);
    }
}

void expectStatus(Status actual, Status expected, const char* label)
{
    expectEqual(static_cast<int>(actual), static_cast<int>(expected), label);
}

// WAV のバイト列を組み立てる。RIFF のサイズ欄は parse() が見ないので 0 のままでよい。
class WavBuilder {
public:
    WavBuilder()
    {
        bytes_ = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    }

    // fmt チャンク (16 バイト)。
    WavBuilder& fmt(uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits)
    {
        std::vector<uint8_t> body;
        put16(body, format);
        put16(body, channels);
        put32(body, rate);
        put32(body, rate * channels * bits / 8);  // byte rate
        put16(body, static_cast<uint16_t>(channels * bits / 8));  // block align
        put16(body, bits);
        return chunk("fmt ", body);
    }

    // id のチャンクを足す。奇数サイズなら詰め物 (1 バイト) も足す。
    WavBuilder& chunk(const char* id, const std::vector<uint8_t>& body)
    {
        bytes_.insert(bytes_.end(), id, id + 4);
        put32(bytes_, static_cast<uint32_t>(body.size()));
        bytes_.insert(bytes_.end(), body.begin(), body.end());
        if (body.size() & 1) {
            bytes_.push_back(0xEE);
        }
        return *this;
    }

    // サイズ欄だけ size にしたチャンクのヘッダを足す (本体は body)。
    WavBuilder& rawChunk(const char* id, uint32_t size, const std::vector<uint8_t>& body)
    {
        bytes_.insert(bytes_.end(), id, id + 4);
        put32(bytes_, size);
        bytes_.insert(bytes_.end(), body.begin(), body.end());
        return *this;
    }

    const std::vector<uint8_t>& bytes() const
    {
        return bytes_;
    }

private:
    static void put16(std::vector<uint8_t>& v, uint16_t x)
    {
        v.push_back(static_cast<uint8_t>(x & 0xFF));
        v.push_back(static_cast<uint8_t>(x >> 8));
    }
    static void put32(std::vector<uint8_t>& v, uint32_t x)
    {
        for (int i = 0; i < 4; ++i) {
            v.push_back(static_cast<uint8_t>((x >> (8 * i)) & 0xFF));
        }
    }

    std::vector<uint8_t> bytes_;
};

std::vector<uint8_t> samples(size_t bytes)
{
    std::vector<uint8_t> v(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        v[i] = static_cast<uint8_t>(i + 1);
    }
    return v;
}

Status run(const std::vector<uint8_t>& bytes, Pcm& out)
{
    return parse(bytes.data(), bytes.size(), kRate, out);
}

void testOk()
{
    WavBuilder b;
    b.fmt(1, 1, kRate, 16).chunk("data", samples(10));
    const auto& bytes = b.bytes();
    Pcm out;
    expectStatus(run(bytes, out), Status::Ok, "ok status");
    expectEqual(static_cast<long long>(out.samples), 5, "ok samples = data size / 2");
    // RIFF ヘッダ 12 + fmt 8+16 + data のヘッダ 8 = 44 バイト目から本体
    expectTrue(out.data == bytes.data() + 44, "ok data points at the data chunk body");
    expectEqual(out.data[0], 1, "ok first sample byte");
    expectEqual(out.format.rate, kRate, "ok format rate");
}

void testOddSampleBytes()
{
    // data のサイズが奇数なら samples は切り捨て (最後の 1 バイトは使わない)。
    WavBuilder b;
    b.fmt(1, 1, kRate, 16).chunk("data", samples(7));
    Pcm out;
    expectStatus(run(b.bytes(), out), Status::Ok, "odd data status");
    expectEqual(static_cast<long long>(out.samples), 3, "odd data samples");
}

void testSkipsOtherChunks()
{
    // fmt の前に LIST (奇数サイズ: 詰め物つき)、fmt と data の間に別のチャンク。
    WavBuilder b;
    b.chunk("LIST", samples(5)).fmt(1, 1, kRate, 16).chunk("fact", samples(4)).chunk("data", samples(8));
    const auto& bytes = b.bytes();
    Pcm out;
    expectStatus(run(bytes, out), Status::Ok, "skip chunks status");
    expectEqual(static_cast<long long>(out.samples), 4, "skip chunks samples");
    // 12 + LIST (8+5+1) + fmt (8+16) + fact (8+4) + data のヘッダ 8 = 70
    expectTrue(out.data == bytes.data() + 70, "skip chunks data offset (odd chunk padded)");
}

void testFmtLargerThan16()
{
    // fmt が 18 バイト (cbSize つき) でも読める。
    WavBuilder b;
    std::vector<uint8_t> body = {1, 0, 1, 0, 0xC0, 0x5D, 0, 0, 0x80, 0xBB, 0, 0, 2, 0, 16, 0, 0, 0};
    b.chunk("fmt ", body).chunk("data", samples(4));
    Pcm out;
    expectStatus(run(b.bytes(), out), Status::Ok, "fmt 18 bytes status");
    expectEqual(static_cast<long long>(out.samples), 2, "fmt 18 bytes samples");
}

void testNotRiff()
{
    Pcm out;
    expectStatus(parse(nullptr, 100, kRate, out), Status::NotRiff, "nullptr");

    WavBuilder b;
    b.fmt(1, 1, kRate, 16).chunk("data", samples(4));
    std::vector<uint8_t> bytes = b.bytes();
    expectStatus(parse(bytes.data(), 11, kRate, out), Status::NotRiff, "shorter than 12 bytes");
    expectStatus(parse(bytes.data(), 0, kRate, out), Status::NotRiff, "empty");

    std::vector<uint8_t> riff = bytes;
    riff[0]                   = 'X';
    expectStatus(run(riff, out), Status::NotRiff, "not RIFF");

    std::vector<uint8_t> wave = bytes;
    wave[8]                   = 'A';
    expectStatus(run(wave, out), Status::NotRiff, "not WAVE");
    expectTrue(out.data == nullptr && out.samples == 0, "NotRiff leaves out empty");
    expectEqual(out.format.format, 0, "NotRiff leaves format empty");
}

void testUnsupported()
{
    struct Case {
        uint16_t format;
        uint16_t channels;
        uint32_t rate;
        uint16_t bits;
        const char* label;
    };
    const Case cases[] = {
        {1, 2, kRate, 16, "stereo"},
        {1, 1, kRate, 8, "8-bit"},
        {1, 1, 16000, 16, "16 kHz"},
        {3, 1, kRate, 16, "format 3 (float)"},
    };
    for (const auto& c : cases) {
        WavBuilder b;
        b.fmt(c.format, c.channels, c.rate, c.bits).chunk("data", samples(4));
        Pcm out;
        expectStatus(run(b.bytes(), out), Status::Unsupported, c.label);
        expectEqual(out.format.format, c.format, "Unsupported keeps format");
        expectEqual(out.format.channels, c.channels, "Unsupported keeps channels");
        expectEqual(out.format.rate, c.rate, "Unsupported keeps rate");
        expectEqual(out.format.bits, c.bits, "Unsupported keeps bits");
        expectTrue(out.data == nullptr, "Unsupported has no data");
    }
}

void testNotFound()
{
    Pcm out;
    {
        // data が fmt より前
        WavBuilder b;
        b.chunk("data", samples(4)).fmt(1, 1, kRate, 16);
        expectStatus(run(b.bytes(), out), Status::NotFound, "data before fmt");
        expectTrue(out.data == nullptr, "data before fmt has no data");
    }
    {
        // data チャンクが無い
        WavBuilder b;
        b.fmt(1, 1, kRate, 16);
        expectStatus(run(b.bytes(), out), Status::NotFound, "no data chunk");
    }
    {
        // fmt チャンクが無い
        WavBuilder b;
        b.chunk("LIST", samples(4));
        expectStatus(run(b.bytes(), out), Status::NotFound, "no fmt chunk");
    }
    {
        // data のサイズがバッファを超える (途中で切れている)
        WavBuilder b;
        b.fmt(1, 1, kRate, 16).rawChunk("data", 100, samples(4));
        expectStatus(run(b.bytes(), out), Status::NotFound, "data larger than buffer");
    }
    {
        // サイズ欄が 0xFFFFFFFF でも桁あふれしない
        WavBuilder b;
        b.fmt(1, 1, kRate, 16).rawChunk("data", 0xFFFFFFFFu, samples(4));
        expectStatus(run(b.bytes(), out), Status::NotFound, "data size 0xFFFFFFFF");
    }
    {
        // fmt が 16 バイト未満なら fmt として扱わない
        WavBuilder b;
        b.chunk("fmt ", samples(14)).chunk("data", samples(4));
        expectStatus(run(b.bytes(), out), Status::NotFound, "fmt shorter than 16 bytes");
    }
    {
        // チャンクのヘッダが 8 バイトに満たない端数で終わる
        WavBuilder b;
        b.fmt(1, 1, kRate, 16);
        std::vector<uint8_t> bytes = b.bytes();
        bytes.insert(bytes.end(), {'d', 'a', 't', 'a', 4});
        expectStatus(run(bytes, out), Status::NotFound, "truncated chunk header");
    }
    {
        // 奇数サイズのチャンクで終わり、詰め物の 1 バイトが無い (pos が len を 1 超える)。
        // 範囲外を読まずに NotFound で終わること (len - pos の巻き戻りの回帰テスト)。
        WavBuilder b;
        b.fmt(1, 1, kRate, 16);
        std::vector<uint8_t> bytes = b.bytes();
        bytes.insert(bytes.end(), {'L', 'I', 'S', 'T', 1, 0, 0, 0, 0x42});
        expectStatus(run(bytes, out), Status::NotFound, "odd chunk without padding at the end");
    }
}

}  // namespace

int main()
{
    testOk();
    testOddSampleBytes();
    testSkipsOtherChunks();
    testFmtLargerThan16();
    testNotRiff();
    testUnsupported();
    testNotFound();
    std::cout << "wav_test: ok\n";
    return 0;
}
