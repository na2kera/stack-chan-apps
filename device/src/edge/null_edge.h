// edge 無しのスタブ (-DPHOTOBOOTH_NO_EDGE で選ぶ)。常に offline で、何も送らない。
// ステップ1の挙動 (固定 URL の QR) を再現する。
#pragma once

#include <cstdio>

#include "edge/edge_client.h"

namespace edge {

class NullEdge : public EdgeClient {
 public:
  bool isOnline() override { return false; }
  void sessionStart(const app::Session&) override {}
  bool offerFrame(const app::Session&, const camera_fb_t&, int, int, Phase) override {
    return false;
  }
  bool pollResult(FrameResult&) override { return false; }
  void sessionTimeout(const app::Session&) override {}
  bool pollTimeout(bool&, bool&) override { return false; }
  bool fetchCandidate(const app::Session&, uint8_t*&, size_t&) override { return false; }
  void reviewDecision(const app::Session&, bool) override {}
  bool pollPhotoReady(PhotoInfo&) override { return false; }
  void sessionCancel(const app::Session&) override {}
  const char* lastError() override { return "edge無効ビルド (PHOTOBOOTH_NO_EDGE)"; }
  void diagnostics(Diagnostics& out) override {
    out = Diagnostics{};
    snprintf(out.last_error, sizeof(out.last_error), "%s", lastError());
  }
  void reconnect() override {}
};

}  // namespace edge
