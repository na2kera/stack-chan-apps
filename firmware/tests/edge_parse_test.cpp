/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の net/edge_parse (edge の応答の解釈。docs/design/testable-logic-step1.md §4) をホストで確かめる。
#include <apps/app_photobooth/net/edge_parse.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

using photobooth::net::FrameResult;
using photobooth::net::Hint;
using photobooth::net::parseFrameResult;
using photobooth::net::parseHint;
using photobooth::net::parsePhotoInfo;
using photobooth::net::ParseStatus;
using photobooth::net::PhotoInfo;

constexpr const char* kSid = "123e4567-e89b-42d3-a456-426614174000";

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

void expectStatus(ParseStatus actual, ParseStatus expected, const char* label)
{
    expectEqual(static_cast<int>(actual), static_cast<int>(expected), label);
}

ParseStatus frame(const std::string& json, FrameResult& out, uint32_t frame_id = 7)
{
    return parseFrameResult(json.data(), json.size(), kSid, frame_id, out);
}

ParseStatus photo(const std::string& json, PhotoInfo& out, const char** missing = nullptr)
{
    return parsePhotoInfo(json.data(), json.size(), out, missing);
}

std::string frameJson(const std::string& extra)
{
    return std::string(R"({"session_id":")") + kSid + R"(","frame_id":7)" + extra + "}";
}

void testFrameAllFields()
{
    FrameResult fr;
    const std::string json = frameJson(
        R"(,"dropped":true,"face_count":3,"target_face_count":2,"all_in_frame":true,"all_eyes_open":true,)"
        R"("all_smiling":true,"servo_dx":-15,"servo_dy":20,"hint":"closer","accepted":true,"latency_ms":123)");
    expectStatus(frame(json, fr), ParseStatus::Ok, "frame all fields status");
    expectTrue(fr.valid, "frame valid");
    expectEqual(fr.frame_id, 7, "frame_id");
    expectTrue(fr.dropped, "dropped");
    expectEqual(fr.face_count, 3, "face_count");
    expectEqual(fr.target_face_count, 2, "target_face_count");
    expectTrue(fr.all_in_frame, "all_in_frame");
    expectTrue(fr.all_eyes_open, "all_eyes_open");
    expectTrue(fr.all_smiling, "all_smiling");
    expectEqual(fr.servo_dx, -15, "servo_dx");
    expectEqual(fr.servo_dy, 20, "servo_dy");
    expectTrue(fr.hint == Hint::Closer, "hint closer");
    expectTrue(fr.accepted, "accepted");
    expectEqual(fr.latency_ms, 123, "latency_ms");
}

void testFrameHint()
{
    FrameResult fr;
    expectStatus(frame(frameJson(R"(,"hint":"too_many")"), fr), ParseStatus::Ok, "hint too_many status");
    expectTrue(fr.hint == Hint::TooMany, "hint too_many");
    expectStatus(frame(frameJson(R"(,"hint":null)"), fr), ParseStatus::Ok, "hint null status");
    expectTrue(fr.hint == Hint::None, "hint null");
    expectStatus(frame(frameJson(R"(,"hint":"other")"), fr), ParseStatus::Ok, "hint unknown status");
    expectTrue(fr.hint == Hint::None, "hint unknown");
    expectStatus(frame(frameJson(""), fr), ParseStatus::Ok, "hint missing status");
    expectTrue(fr.hint == Hint::None, "hint missing");

    expectTrue(parseHint(nullptr) == Hint::None, "parseHint nullptr");
    expectTrue(parseHint("closer") == Hint::Closer, "parseHint closer");
    expectTrue(parseHint("too_many") == Hint::TooMany, "parseHint too_many");
    expectTrue(parseHint("") == Hint::None, "parseHint empty");
}

void testFrameClamp()
{
    FrameResult fr;
    expectStatus(frame(frameJson(R"(,"face_count":300,"target_face_count":-4,"latency_ms":70000)"), fr),
                 ParseStatus::Ok, "clamp status");
    expectEqual(fr.face_count, 255, "face_count 300 -> 255");
    expectEqual(fr.target_face_count, 0, "target_face_count -4 -> 0");
    expectEqual(fr.latency_ms, 65535, "latency_ms 70000 -> 65535");
}

void testFrameDefaults()
{
    // session_id / frame_id 以外が欠けていても既定値で Ok。
    FrameResult fr;
    expectStatus(frame(frameJson(""), fr), ParseStatus::Ok, "defaults status");
    expectTrue(fr.valid, "defaults valid");
    expectTrue(!fr.dropped && !fr.accepted && !fr.all_in_frame && !fr.all_eyes_open && !fr.all_smiling,
               "defaults bools false");
    expectEqual(fr.face_count, 0, "defaults face_count");
    expectEqual(fr.servo_dx, 0, "defaults servo_dx");
    expectEqual(fr.latency_ms, 0, "defaults latency_ms");
}

void testFrameMismatch()
{
    FrameResult fr;
    fr.face_count = 9;  // Mismatch / BadJson のときは out を変えない
    expectStatus(frame(R"({"session_id":"other","frame_id":7})", fr), ParseStatus::Mismatch, "session_id differs");
    expectStatus(frame(frameJson(""), fr, 8), ParseStatus::Mismatch, "frame_id differs");
    expectStatus(frame(R"({"frame_id":7})", fr), ParseStatus::Mismatch, "session_id missing");
    expectEqual(fr.face_count, 9, "out untouched on mismatch");
    expectTrue(!fr.valid, "out not valid on mismatch");

    expectStatus(frame(R"({"session_id":)", fr), ParseStatus::BadJson, "broken json");
    expectStatus(frame("", fr), ParseStatus::BadJson, "empty body");
    expectEqual(fr.face_count, 9, "out untouched on bad json");
}

void testFrameUsesLength()
{
    // len より後ろは読まない (本文のあとに別の文字があっても len までで解釈する)。
    const std::string json = frameJson(R"(,"face_count":2)") + "garbage";
    FrameResult fr;
    expectStatus(parseFrameResult(json.data(), json.size() - 7, kSid, 7, fr), ParseStatus::Ok, "len respected");
    expectEqual(fr.face_count, 2, "len respected face_count");
}

void testPhotoPending()
{
    PhotoInfo info;
    expectStatus(photo(R"({"status":"pending"})", info), ParseStatus::Ok, "pending status");
    expectTrue(info.status == PhotoInfo::Status::Pending, "pending");
}

void testPhotoReady()
{
    PhotoInfo info;
    expectStatus(photo(R"({"status":"ready","photo_url":"https://g/p/1","share_url":"https://g/s/1",)"
                       R"("expires_at":"2026-09-30T22:00:00+09:00"})",
                       info),
                 ParseStatus::Ok, "ready status");
    expectTrue(info.status == PhotoInfo::Status::Ready, "ready");
    expectStr(info.photo_url, "https://g/p/1", "ready photo_url");
    expectStr(info.share_url, "https://g/s/1", "ready share_url");
    expectStr(info.expires_at, "2026-09-30T22:00:00+09:00", "ready expires_at");
    expectStr(info.reason, "", "ready reason empty");
}

void testPhotoBadResponse()
{
    struct Case {
        const char* json;
        const char* missing;
    };
    const Case cases[] = {
        {R"({"status":"ready","share_url":"s","expires_at":"e"})", "photo_url"},
        {R"({"status":"ready","photo_url":"p","share_url":"","expires_at":"e"})", "share_url"},
        {R"({"status":"ready","photo_url":"p","share_url":"s"})", "expires_at"},
        {R"({"status":"ready"})", "photo_url"},
    };
    for (const auto& c : cases) {
        PhotoInfo info;
        const char* missing = "";
        expectStatus(photo(c.json, info, &missing), ParseStatus::BadResponse, c.json);
        expectTrue(info.status == PhotoInfo::Status::Error, "bad_response is Error");
        expectStr(info.reason, "bad_response", "bad_response reason");
        expectStr(missing, c.missing, "bad_response missing field");
        expectStr(info.photo_url, "", "bad_response has no photo_url");
    }
    // missing を渡さなくてもよい
    PhotoInfo info;
    expectStatus(photo(R"({"status":"ready"})", info), ParseStatus::BadResponse, "bad_response without missing");
}

void testPhotoBadPhotoUrl()
{
    const std::string long_url(256, 'a');  // photo_url[256] には 255 文字まで
    const std::string ok_url(255, 'b');
    PhotoInfo info;
    expectStatus(photo(R"({"status":"ready","photo_url":")" + long_url + R"(","share_url":"s","expires_at":"e"})",
                       info),
                 ParseStatus::BadPhotoUrl, "photo_url 256 chars");
    expectTrue(info.status == PhotoInfo::Status::Error, "bad_photo_url is Error");
    expectStr(info.reason, "bad_photo_url", "bad_photo_url reason");
    expectStr(info.photo_url, "", "bad_photo_url has no photo_url");

    expectStatus(photo(R"({"status":"ready","photo_url":"p","share_url":")" + long_url + R"(","expires_at":"e"})",
                       info),
                 ParseStatus::BadPhotoUrl, "share_url 256 chars");
    expectStatus(photo(R"({"status":"ready","photo_url":"p","share_url":"s","expires_at":")" +
                           std::string(32, '9') + R"("})",
                       info),
                 ParseStatus::BadPhotoUrl, "expires_at 32 chars");

    expectStatus(photo(R"({"status":"ready","photo_url":")" + ok_url + R"(","share_url":"s","expires_at":"e"})",
                       info),
                 ParseStatus::Ok, "photo_url 255 chars fits");
    expectEqual(static_cast<long long>(std::strlen(info.photo_url)), 255, "photo_url 255 chars copied");
}

void testPhotoError()
{
    PhotoInfo info;
    expectStatus(photo(R"({"status":"error","reason":"upload_failed"})", info), ParseStatus::Ok, "error status");
    expectTrue(info.status == PhotoInfo::Status::Error, "error");
    expectStr(info.reason, "upload_failed", "error reason copied");

    expectStatus(photo(R"({"status":"error"})", info), ParseStatus::Ok, "error without reason status");
    expectStr(info.reason, "error", "error without reason -> \"error\"");

    // 知らない status・status 無しも Error として扱う
    expectStatus(photo(R"({"status":"weird","reason":"x"})", info), ParseStatus::Ok, "unknown status");
    expectTrue(info.status == PhotoInfo::Status::Error, "unknown status is Error");
    expectStr(info.reason, "x", "unknown status reason");
    expectStatus(photo("{}", info), ParseStatus::Ok, "no status");
    expectStr(info.reason, "error", "no status reason");

    // reason が配列に収まらなければ切り詰める (reason[32])
    expectStatus(photo(R"({"status":"error","reason":")" + std::string(40, 'r') + R"("})", info), ParseStatus::Ok,
                 "long reason");
    expectEqual(static_cast<long long>(std::strlen(info.reason)), 31, "long reason truncated to 31");
}

void testPhotoBadJson()
{
    PhotoInfo info;
    expectStatus(photo("not json", info), ParseStatus::BadJson, "photo bad json");
    expectStatus(photo("", info), ParseStatus::BadJson, "photo empty body");
}

}  // namespace

int main()
{
    testFrameAllFields();
    testFrameHint();
    testFrameClamp();
    testFrameDefaults();
    testFrameMismatch();
    testFrameUsesLength();
    testPhotoPending();
    testPhotoReady();
    testPhotoBadResponse();
    testPhotoBadPhotoUrl();
    testPhotoError();
    testPhotoBadJson();
    std::cout << "edge_parse_test: ok\n";
    return 0;
}
