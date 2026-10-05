/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 頭部タッチと画面タップを Event に正規化する (device/src/hal/input の移植。app_photobooth から切り出した。
// docs/design/app-roulette.md §3)。
//
// 頭部タッチは GetHAL().onHeadPetGesture (headtouch タスクから emit)、画面は各アプリの view/ の LVGL
// クリックコールバック (LVGL タスクから push) で届く。どちらも別スレッドなので FreeRTOS のキューに
// 待たずに (0 tick で) 積むだけにし、溢れたら捨てて数える。LVGL タスクがここでブロックすることはない。
// 状態機械は onRunning のスレッドで poll() する。
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shared::hw {

struct Event {
    enum class Kind : uint8_t {
        None,
        ScreenTap,  // 画面のボタン以外の場所 (待機画面の開始用)
        HeadTap,    // 頭部タッチ: Press → Release (スワイプを挟まない)
        Button,     // 画面のボタン・タップ領域。index はアプリが決める (photobooth: 下のボタン左から 0, 1。
                    // roulette: リールの列 0, 1, 2)
    };
    Kind kind       = Kind::None;
    int index       = -1;  // Button のときだけ有効
    uint32_t screen = 0;   // 画面タップ・ボタンが押された画面の世代 (アプリの View が決める番号)
};

class Input {
public:
    Input();
    ~Input();
    Input(const Input&)            = delete;
    Input& operator=(const Input&) = delete;

    // onHeadPetGesture に接続する。
    void begin();
    // 接続を外してキューを捨てる。
    void end();

    // LVGL のクリックコールバックから呼ぶ (LVGL タスク)。ブロックしない。
    void pushScreenTap(uint32_t screen);
    void pushButton(uint32_t screen, int index);

    // 1 件取り出す (待たない)。無ければ Kind::None。
    Event poll();

    // 溜まっているイベントを捨てる (画面を切り替えた直後など)。
    void clear();

private:
    void push(const Event& ev);
    void onHeadGesture(int gesture);

    static constexpr size_t kQueueSize = 8;
    QueueHandle_t queue_ = nullptr;
    std::atomic<uint32_t> dropped_{0};   // キューが一杯で捨てた数
    std::atomic<bool> pressed_{false};   // 頭部: Press を受けてまだ Release もスワイプも来ていない
    size_t conn_id_ = 0;
    bool connected_ = false;
};

}  // namespace shared::hw
