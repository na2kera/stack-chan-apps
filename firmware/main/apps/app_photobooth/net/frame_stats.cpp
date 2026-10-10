/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "frame_stats.h"

#include <algorithm>
#include <limits>

namespace photobooth::net::frame {

namespace {

uint32_t average(uint64_t sum, uint32_t n)
{
    return n == 0 ? 0 : static_cast<uint32_t>(sum / n);
}

uint32_t clampToU32(size_t v)
{
    return v > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                    : static_cast<uint32_t>(v);
}

}  // namespace

Format selectFormat(bool jpeg_enabled)
{
    return jpeg_enabled ? Format::Jpeg : Format::Rgb565;
}

const char* formatHeader(Format f)
{
    return f == Format::Jpeg ? "jpeg" : "rgb565";
}

bool injectEncodeFailure(uint32_t n, uint32_t every)
{
    return every != 0 && n != 0 && n % every == 0;
}

void Stats::addEncoded(uint32_t encode_ms, size_t jpeg_bytes)
{
    const uint32_t bytes = clampToU32(jpeg_bytes);
    ++encoded_;
    encode_sum_ += encode_ms;
    encode_max_ = std::max(encode_max_, encode_ms);
    jpeg_sum_ += bytes;
    jpeg_max_ = std::max(jpeg_max_, bytes);
}

void Stats::addEncodeFailure()
{
    ++encode_failures_;
}

void Stats::addSent(uint32_t rtt_ms, uint32_t edge_ms)
{
    ++sent_;
    rtt_sum_ += rtt_ms;
    rtt_max_ = std::max(rtt_max_, rtt_ms);
    edge_sum_ += edge_ms;
}

void Stats::addSendFailure()
{
    ++send_failures_;
}

void Stats::addSkipped()
{
    ++skipped_;
}

bool Stats::empty() const
{
    return sent_ == 0 && send_failures_ == 0 && encode_failures_ == 0 && skipped_ == 0 && encoded_ == 0;
}

Summary Stats::summarize(uint32_t dt_ms) const
{
    Summary s;
    s.sent            = sent_;
    s.send_failures   = send_failures_;
    s.encode_failures = encode_failures_;
    s.skipped         = skipped_;
    s.attempts        = sent_ + send_failures_ + encode_failures_;
    s.fps             = dt_ms == 0 ? 0.0f : static_cast<float>(sent_) * 1000.0f / static_cast<float>(dt_ms);
    s.rtt_avg_ms      = average(rtt_sum_, sent_);
    s.rtt_max_ms      = rtt_max_;
    s.edge_avg_ms     = average(edge_sum_, sent_);
    s.encoded         = encoded_;
    s.encode_avg_ms   = average(encode_sum_, encoded_);
    s.encode_max_ms   = encode_max_;
    s.jpeg_avg_bytes  = average(jpeg_sum_, encoded_);
    s.jpeg_max_bytes  = jpeg_max_;
    return s;
}

void Stats::reset()
{
    *this = Stats{};
}

}  // namespace photobooth::net::frame
