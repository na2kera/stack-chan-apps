/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "edge_parse.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace photobooth::net {

namespace {

void copyStr(char* dst, size_t n, const char* src)
{
    snprintf(dst, n, "%s", src != nullptr ? src : "");
}

uint8_t clampU8(int v)
{
    return static_cast<uint8_t>(std::min(std::max(v, 0), 255));
}

}  // namespace

Hint parseHint(const char* h)
{
    if (h == nullptr) return Hint::None;
    if (strcmp(h, "closer") == 0) return Hint::Closer;
    if (strcmp(h, "too_many") == 0) return Hint::TooMany;
    return Hint::None;
}

ParseStatus parseFrameResult(const char* json, size_t len, const char* expect_sid, uint32_t expect_frame_id,
                             FrameResult& out)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
        return ParseStatus::BadJson;
    }
    const char* sid         = doc["session_id"] | "";
    const uint32_t frame_id = doc["frame_id"] | 0UL;
    if (strcmp(sid, expect_sid) != 0 || frame_id != expect_frame_id) {
        return ParseStatus::Mismatch;
    }

    FrameResult fr;
    fr.valid             = true;
    fr.frame_id          = frame_id;
    fr.dropped           = doc["dropped"] | false;
    fr.face_count        = clampU8(doc["face_count"] | 0);
    fr.target_face_count = clampU8(doc["target_face_count"] | 0);
    fr.all_in_frame      = doc["all_in_frame"] | false;
    fr.all_eyes_open     = doc["all_eyes_open"] | false;
    fr.all_smiling       = doc["all_smiling"] | false;
    fr.servo_dx          = doc["servo_dx"] | 0;
    fr.servo_dy          = doc["servo_dy"] | 0;
    fr.hint              = parseHint(doc["hint"] | static_cast<const char*>(nullptr));
    fr.accepted          = doc["accepted"] | false;
    fr.latency_ms        = static_cast<uint16_t>(std::min<uint32_t>(doc["latency_ms"] | 0UL, 65535));
    out                  = fr;
    return ParseStatus::Ok;
}

ParseStatus parsePhotoInfo(const char* json, size_t len, PhotoInfo& out, const char** missing)
{
    out = PhotoInfo{};
    JsonDocument doc;
    if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
        return ParseStatus::BadJson;
    }
    const char* status = doc["status"] | "";
    if (strcmp(status, "pending") == 0) {
        out.status = PhotoInfo::Status::Pending;
        return ParseStatus::Ok;
    }
    if (strcmp(status, "ready") == 0) {
        const char* photo_url  = doc["photo_url"] | "";
        const char* share_url  = doc["share_url"] | "";
        const char* expires_at = doc["expires_at"] | "";
        if (photo_url[0] == '\0' || share_url[0] == '\0' || expires_at[0] == '\0') {
            // ready なら 3 つとも必須 (protocol.md)。欠けた応答で QR を出さない。
            out.status = PhotoInfo::Status::Error;
            copyStr(out.reason, sizeof(out.reason), "bad_response");
            if (missing != nullptr) {
                *missing = photo_url[0] == '\0'   ? "photo_url"
                           : share_url[0] == '\0' ? "share_url"
                                                  : "expires_at";
            }
            return ParseStatus::BadResponse;
        }
        if (strlen(photo_url) >= sizeof(out.photo_url) || strlen(share_url) >= sizeof(out.share_url) ||
            strlen(expires_at) >= sizeof(out.expires_at)) {
            // 切り詰めた URL の QR は出さない (spec §9「QR を捏造しない」)
            out.status = PhotoInfo::Status::Error;
            copyStr(out.reason, sizeof(out.reason), "bad_photo_url");
            return ParseStatus::BadPhotoUrl;
        }
        out.status = PhotoInfo::Status::Ready;
        copyStr(out.photo_url, sizeof(out.photo_url), photo_url);
        copyStr(out.share_url, sizeof(out.share_url), share_url);
        copyStr(out.expires_at, sizeof(out.expires_at), expires_at);
        return ParseStatus::Ok;
    }
    out.status = PhotoInfo::Status::Error;
    copyStr(out.reason, sizeof(out.reason), doc["reason"] | "error");
    return ParseStatus::Ok;
}

}  // namespace photobooth::net
