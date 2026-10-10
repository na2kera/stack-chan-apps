/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// frame の形式の選択と送信統計の集計 (docs/design/step6-cloud-device.md §3.3, §3.4)。ロジック層: ESP-IDF・
// FreeRTOS のヘッダを含めない。時刻・所要時間・JPEG の長さは呼び出し側 (net/http_edge_client.cpp) が測って渡す。
//
//   - 送る形式 (RGB565 / JPEG) と X-Format の値
//   - 符号化失敗の注入 (試験 22 のデバッグ定義)
//   - logStats() の 5 秒窓の集計 (送信 fps、往復時間、符号化の所要と JPEG の大きさ、失敗の内訳)
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::net::frame {

enum class Format : uint8_t { Rgb565, Jpeg };

// config::FRAME_FORMAT_JPEG から送る形式を決める。
Format selectFormat(bool jpeg_enabled);
// X-Format ヘッダの値 (docs/protocol.md)。
const char* formatHeader(Format f);

// 符号化失敗を注入するか。n は符号化を試みた通し番号 (1 から)、every は 0 で無効、
// N なら N 回に 1 回 (n が N の倍数のとき) 失敗させる。
bool injectEncodeFailure(uint32_t n, uint32_t every);

// logStats() の 1 窓分の要約。平均は件数 0 なら 0。
struct Summary {
    uint32_t sent            = 0;  // 200 で frame_result を読めたフレーム
    uint32_t send_failures   = 0;  // 送ったが失敗したフレーム (通信失敗・200 以外・応答不正)
    uint32_t encode_failures = 0;  // 符号化に失敗して破棄したフレーム (確保失敗を含む)
    uint32_t skipped         = 0;  // offline で送らずに捨てたフレーム
    uint32_t attempts        = 0;  // 送信を試みたフレーム = sent + send_failures + encode_failures (§3.4 の分母)
    float fps                = 0;  // sent ÷ 窓の長さ
    uint32_t rtt_avg_ms      = 0;  // 成功したフレームの往復 (符号化を含まない)
    uint32_t rtt_max_ms      = 0;
    uint32_t edge_avg_ms     = 0;  // edge の latency_ms
    uint32_t encoded         = 0;  // 符号化に成功したフレーム (送信の成否は問わない)
    uint32_t encode_avg_ms   = 0;
    uint32_t encode_max_ms   = 0;
    uint32_t jpeg_avg_bytes  = 0;
    uint32_t jpeg_max_bytes  = 0;
};

// 送信統計の窓。net タスクだけが触る (排他しない)。
class Stats {
public:
    void addEncoded(uint32_t encode_ms, size_t jpeg_bytes);
    void addEncodeFailure();
    void addSent(uint32_t rtt_ms, uint32_t edge_ms);
    void addSendFailure();
    void addSkipped();

    // 窓の中で何か起きたか (ログを出すか)。
    bool empty() const;
    // dt_ms は窓の長さ (0 なら fps は 0)。
    Summary summarize(uint32_t dt_ms) const;
    void reset();

private:
    uint32_t sent_            = 0;
    uint32_t send_failures_   = 0;
    uint32_t encode_failures_ = 0;
    uint32_t skipped_         = 0;
    uint64_t rtt_sum_         = 0;
    uint32_t rtt_max_         = 0;
    uint64_t edge_sum_        = 0;
    uint32_t encoded_         = 0;
    uint64_t encode_sum_      = 0;
    uint32_t encode_max_      = 0;
    uint64_t jpeg_sum_        = 0;
    uint32_t jpeg_max_        = 0;
};

}  // namespace photobooth::net::frame
