/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge の応答本文 (JSON) を FrameResult / PhotoInfo に変換する (docs/design/testable-logic-step1.md §4)。
// http_edge_client.cpp から切り出した純粋な関数。通信・ログ・統計は呼び出し側 (EdgeWorker) が持ち、
// ParseStatus を見て今までと同じ文言でログを出す。ArduinoJson だけに依存する
// (firmware/tests/edge_parse_test.cpp でホストでテストする)。
#pragma once

#include <cstddef>
#include <cstdint>

#include "edge_types.h"

namespace photobooth::net {

enum class ParseStatus : uint8_t {
    Ok,
    BadJson,      // JSON として読めない
    Mismatch,     // frame_result の session_id / frame_id が送ったものと違う
    BadResponse,  // photo が ready なのに photo_url / share_url / expires_at のどれかが空
    BadPhotoUrl,  // photo が ready なのに、どれかが PhotoInfo の配列に収まらない
};

// frame の応答本文。session_id / frame_id が送ったものと違えば Mismatch (out は変えない)。
// Ok のとき out を全項目埋める (valid = true。欠けた項目は既定値)。
ParseStatus parseFrameResult(const char* json, size_t len, const char* expect_sid, uint32_t expect_frame_id,
                             FrameResult& out);

// photo の応答本文。pending なら out.status = Pending で Ok。error (または不明な status) なら
// out.status = Error、reason をコピー (無ければ "error") して Ok。
// ready で photo_url / share_url / expires_at のどれかが空なら BadResponse、
// どれかが PhotoInfo の配列に収まらなければ BadPhotoUrl (どちらも out.status = Error で reason もその場で入れる)。
// missing を渡すと、BadResponse のとき欠けていた項目名 ("photo_url" など。ログ用) を入れる。
ParseStatus parsePhotoInfo(const char* json, size_t len, PhotoInfo& out, const char** missing = nullptr);

// frame_result の hint。nullptr や知らない値は None。
Hint parseHint(const char* h);

}  // namespace photobooth::net
