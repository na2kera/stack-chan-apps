// edge が無いステップ1用のスタブ。常に offline で、何も送らない。
#pragma once

#include "edge/edge_client.h"

namespace edge {

class NullEdge : public EdgeClient {
 public:
  bool isOnline() override { return false; }
  void sessionStart(const app::Session&) override {}
  bool sendFrame(const app::Session&, const camera_fb_t&) override { return false; }
  bool pollResult(FrameResult&) override { return false; }
  void sessionTimeout(const app::Session&) override {}
  void reviewDecision(const app::Session&, bool) override {}
  bool pollPhotoReady(String&, String&, String&) override { return false; }
  void sessionCancel(const app::Session&) override {}
};

}  // namespace edge
