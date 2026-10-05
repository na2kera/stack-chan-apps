/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 技育スロット (docs/design/app-roulette.md)。Moddable 版 stack-chan-roulette を純正ランチャーから開く
// Mooncake アプリとして移植したもの。
#pragma once
#include <mooncake.h>

#include <cstdint>
#include <memory>

#include "../shared/hw/audio.h"

namespace shared::hw {
class Input;
}  // namespace shared::hw

namespace roulette {
namespace game {
class Slot;
}
namespace hw {
class Lights;
}
namespace view {
class View;
}
}  // namespace roulette

class AppRoulette : public mooncake::AppAbility {
public:
    AppRoulette();
    ~AppRoulette();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // 画面または頭部タップで回し始める (「スタート」の発話と虹色 LED)。
    void startGame(uint32_t now_ms);

    // onOpen で作り、onClose で壊す。
    std::unique_ptr<shared::hw::Input> _input;
    std::unique_ptr<shared::hw::Audio> _audio;
    std::unique_ptr<roulette::hw::Lights> _lights;
    std::unique_ptr<roulette::view::View> _view;
    std::unique_ptr<roulette::game::Slot> _slot;
    shared::hw::Audio::Pcm _voice_start;
    shared::hw::Audio::Pcm _voice_reach;
    bool _reach_active    = false;  // リーチの演出中 (立ち上がり・立ち下がりを見る)
    bool _close_requested = false;  // ホームインジケータのホームボタンが押された
};
