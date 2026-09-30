// 画面タッチ (M5.Touch) と頭部タッチ (TouchSensor) を Event に正規化する。
// ボタンの矩形は知らない (ui の矩形と App が突き合わせる)。
#pragma once

#include <cstdint>

namespace hal {

struct Event {
  enum class Kind : uint8_t { None, ScreenTap, HeadTap };
  Kind kind = Kind::None;
  int16_t x = 0;  // ScreenTap のときだけ有効
  int16_t y = 0;
};

class Input {
 public:
  // M5StackChan.update() を 1 回呼び、その周期で起きたタップを 1 つ返す。
  Event poll();
};

}  // namespace hal
