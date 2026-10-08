/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge の応答を表す型 (frame_result / photo_ready)。edge_client.h から分けた。
// edge_parse (応答の解釈) とホストテストからも使うので、型だけを置き、ESP-IDF のヘッダを含めない。
#pragma once

#include <cstdint>

namespace photobooth::net {

// frame_result の hint (protocol.md)。
enum class Hint : uint8_t { None, Closer, TooMany };

// frame_result (spec §8, protocol.md)。
struct FrameResult {
    bool valid                = false;
    uint32_t frame_id         = 0;
    bool dropped              = false;
    uint8_t face_count        = 0;
    uint8_t target_face_count = 0;
    bool all_in_frame         = false;
    bool all_eyes_open        = false;
    bool all_smiling          = false;
    int servo_dx              = 0;  // 1/10 度。device は Head::nudge() でさらにクランプする
    int servo_dy              = 0;
    Hint hint                 = Hint::None;
    bool accepted             = false;
    uint16_t latency_ms       = 0;  // edge 側の処理時間
};

// PhotoInfo::reason: save の送信回数 (UPLOAD_RETRY) を使い切ったので送らなかった。
inline constexpr const char* kSaveRetryExhausted = "retry_exhausted";

// photo_ready (GET …/photo)。
struct PhotoInfo {
    enum class Status : uint8_t { Pending, Ready, Error };
    Status status       = Status::Pending;
    char photo_url[256] = {};
    char share_url[256] = {};
    char expires_at[32] = {};  // ISO 8601 (+09:00)
    char reason[32]     = {};  // status == Error のとき (例 "upload_failed")
};

}  // namespace photobooth::net
