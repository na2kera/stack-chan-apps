/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "input.h"

#include <hal/hal.h>
#include <mooncake_log.h>

namespace photobooth::hw {

namespace {
constexpr const char* kTag = "PB-Input";
}

Input::Input()
{
    queue_ = xQueueCreate(kQueueSize, sizeof(Event));
    if (queue_ == nullptr) {
        mclog::tagError(kTag, "failed to create event queue");
    }
}

Input::~Input()
{
    end();
    if (queue_ != nullptr) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

void Input::begin()
{
    clear();
    if (!connected_) {
        conn_id_   = GetHAL().onHeadPetGesture.connect([this](HeadPetGesture g) { onHeadGesture(static_cast<int>(g)); });
        connected_ = true;
    }
}

void Input::end()
{
    if (connected_) {
        GetHAL().onHeadPetGesture.disconnect(conn_id_);
        connected_ = false;
    }
    clear();
    const uint32_t dropped = dropped_.exchange(0);
    if (dropped > 0) {
        mclog::tagWarn(kTag, "{} events dropped (queue full)", dropped);
    }
}

void Input::onHeadGesture(int gesture)
{
    // headtouch タスクだけが呼ぶ。Press → Release をタップとし、途中でスワイプになったものは捨てる。
    const auto g = static_cast<HeadPetGesture>(gesture);
    if (g == HeadPetGesture::Press) {
        pressed_ = true;
    } else if (g == HeadPetGesture::SwipeForward || g == HeadPetGesture::SwipeBackward) {
        pressed_ = false;
    } else if (g == HeadPetGesture::Release) {
        if (pressed_.exchange(false)) {
            Event ev;
            ev.kind = Event::Kind::HeadTap;
            push(ev);
        }
    }
}

void Input::pushScreenTap(uint32_t screen)
{
    Event ev;
    ev.kind   = Event::Kind::ScreenTap;
    ev.screen = screen;
    push(ev);
}

void Input::pushButton(uint32_t screen, int index)
{
    Event ev;
    ev.kind   = Event::Kind::Button;
    ev.index  = index;
    ev.screen = screen;
    push(ev);
}

void Input::push(const Event& ev)
{
    // 呼び出し元は LVGL タスク / headtouch タスク。待たずに積み、一杯なら捨てて数える
    // (状態機械は 1 tick に 1 件ずつ消費するので、溢れるのは連打されたときだけ)。
    if (queue_ == nullptr || xQueueSend(queue_, &ev, 0) != pdTRUE) {
        dropped_.fetch_add(1);
    }
}

Event Input::poll()
{
    Event ev;
    if (queue_ == nullptr || xQueueReceive(queue_, &ev, 0) != pdTRUE) {
        return Event{};
    }
    if (ev.kind == Event::Kind::HeadTap) {
        mclog::tagInfo(kTag, "head tap");  // キューから取り出した後 (ロックの外) で出す
    }
    return ev;
}

void Input::clear()
{
    if (queue_ != nullptr) {
        xQueueReset(queue_);
    }
    pressed_ = false;
}

}  // namespace photobooth::hw
