/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge 無しのスタブ (config_local.h が無い、または -DPHOTOBOOTH_NO_EDGE=1 のビルドで使う)。
// 常に offline で、何も送らず、Wi-Fi も起動しない。ステップ1の挙動 (固定 URL の QR) を再現する。
#pragma once

#include <cstdio>

#include "../view/strings.h"
#include "edge_client.h"

namespace photobooth::net {

class NullEdge : public EdgeClient {
public:
    bool begin() override
    {
        return true;
    }
    void end() override
    {
    }
    bool isOnline() override
    {
        return false;
    }
    LinkState linkState() override
    {
        return LinkState::Offline;
    }
    void sessionStart(const Session&) override
    {
    }
    bool offerFrame(const Session&, const hw::FrameView&, int, int, Phase) override
    {
        return false;
    }
    bool pollResult(FrameResult&) override
    {
        return false;
    }
    void sessionTimeout(const Session&) override
    {
    }
    bool pollTimeout(bool&, bool&) override
    {
        return false;
    }
    void requestCandidate(const Session&, uint32_t, bool) override
    {
    }
    bool pollCandidate(bool&, JpegBytes&) override
    {
        return false;
    }
    void reviewDecision(const Session&, bool) override
    {
    }
    bool pollPhotoReady(PhotoInfo&) override
    {
        return false;
    }
    void sessionCancel(const Session&) override
    {
    }
    const char* lastError() override
    {
        return str::kNetDisabledBuild;
    }
    void diagnostics(Diagnostics& out) override
    {
        out = Diagnostics{};
        snprintf(out.last_error, sizeof(out.last_error), "%s", lastError());
    }
    void reconnect() override
    {
    }
};

}  // namespace photobooth::net
