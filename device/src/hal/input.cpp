#include "hal/input.h"

#include <M5StackChan.h>
#include <esp_log.h>

namespace hal {

namespace {
constexpr const char* TAG = "input";
}

Event Input::poll() {
  M5StackChan.update();

  Event ev;
  const auto detail = M5.Touch.getDetail();
  if (detail.wasClicked()) {
    ev.kind = Event::Kind::ScreenTap;
    ev.x = detail.x;
    ev.y = detail.y;
    ESP_LOGD(TAG, "screen tap (%d,%d)", ev.x, ev.y);
    return ev;
  }
  if (M5StackChan.TouchSensor.wasClicked()) {
    ev.kind = Event::Kind::HeadTap;
    ESP_LOGD(TAG, "head tap");
  }
  return ev;
}

}  // namespace hal
