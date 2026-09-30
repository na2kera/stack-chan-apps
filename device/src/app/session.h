// 1 回の撮影セッション (ANNOUNCE〜CAPTURE の 10 秒) の識別子と単調時計。
// ステップ2で edge の session_start / frame / session_timeout に載せる (spec §8)。
#pragma once

#include <cstdint>
#include <cstdio>

#include <esp_random.h>

namespace app {

struct Session {
  char id[37] = {};         // UUID v4 文字列 (36 文字 + NUL)。端末識別子を含めない
  uint32_t frame_id = 0;    // セッション内で単調増加するフレーム番号
  uint32_t started_ms = 0;  // セッション開始時の millis()
  bool active = false;

  // 新しい session_id を振って時計を 0 に戻す。
  void start(uint32_t now_ms) {
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
      const uint32_t r = esp_random();
      b[i] = r & 0xff;
      b[i + 1] = (r >> 8) & 0xff;
      b[i + 2] = (r >> 16) & 0xff;
      b[i + 3] = (r >> 24) & 0xff;
    }
    b[6] = (b[6] & 0x0f) | 0x40;  // version 4
    b[8] = (b[8] & 0x3f) | 0x80;  // RFC 4122 variant
    snprintf(id, sizeof(id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12],
             b[13], b[14], b[15]);
    frame_id = 0;
    started_ms = now_ms;
    active = true;
  }

  void end() { active = false; }

  uint32_t nextFrameId() { return ++frame_id; }

  // セッション開始からの経過 ms (capture_monotonic_ms)。
  uint32_t monotonicMs(uint32_t now_ms) const { return now_ms - started_ms; }
};

}  // namespace app
