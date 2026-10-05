/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_roulette.h"

#include <apps/common/common.h>
#include <esp_random.h>
#include <hal/hal.h>
#include <mooncake_log.h>

#include "../shared/hw/input.h"
#include "assets/rl_assets.h"
#include "game/slot.h"
#include "hw/lights.h"
#include "view/view.h"

using namespace mooncake;
using roulette::game::Slot;

namespace {
// ランチャーの背景色 (紫。純正アプリや Photobooth の黄色と被らない)
constexpr uint32_t kThemeColor = 0x6C5CE7;
// ホームインジケータのボタンと枠 (テーマ色に合わせる)
constexpr uint32_t kHomeButtonColor = 0x6C5CE7;
constexpr uint32_t kHomeBorderColor = 0x1E1650;
}  // namespace

AppRoulette::AppRoulette()
{
    // ランチャーに出る名前とアイコン (アイコンは assets パーティションではなく C 配列)
    setAppInfo().name = "Slot";
    setAppInfo().icon = (void*)&icon_roulette;
    // ランチャーの背景色
    static uint32_t theme_color = kThemeColor;
    setAppInfo().userData       = (void*)&theme_color;
}

AppRoulette::~AppRoulette() = default;

void AppRoulette::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppRoulette::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    _input  = std::make_unique<shared::hw::Input>();
    _audio  = std::make_unique<shared::hw::Audio>();
    _lights = std::make_unique<roulette::hw::Lights>();
    _view   = std::make_unique<roulette::view::View>(*_input);
    _slot   = std::make_unique<Slot>();

    _reach_active    = false;
    _close_requested = false;

    _voice_start = shared::hw::Audio::parseWav(rl_voice_start_start, rl_voice_start_end, "rl_start");
    _voice_reach = shared::hw::Audio::parseWav(rl_voice_reach_start, rl_voice_reach_end, "rl_reach");

    _input->begin();
    if (!_audio->begin()) {
        mclog::tagWarn(getAppInfo().name, "audio unavailable; continue without voice");
    }
    _lights->off();
    _view->begin();
    {
        LvglLockGuard lock;
        view::create_home_indicator([this]() { _close_requested = true; }, kHomeButtonColor, kHomeBorderColor);
    }

    const uint32_t now = GetHAL().millis();
    _slot->advance(now);  // 時刻を覚えるだけ
    _view->render(*_slot);
}

void AppRoulette::startGame(uint32_t now_ms)
{
    const int r0 = static_cast<int>(esp_random() % roulette::game::kSymbolCount);
    const int r1 = static_cast<int>(esp_random() % roulette::game::kSymbolCount);
    const int r2 = static_cast<int>(esp_random() % roulette::game::kSymbolCount);
    _slot->start(r0, r1, r2);
    _reach_active = false;
    mclog::tagInfo(getAppInfo().name, "start ({}, {}, {})", r0, r1, r2);
    _lights->startRainbow(now_ms);
    // 発話を待たずに回す。発話中に再スタートしたら重ねない (移植元と同じ)。
    if (!_audio->isPlaying()) {
        _audio->play(_voice_start);
    }
}

void AppRoulette::onRunning()
{
    const uint32_t now = GetHAL().millis();

    // 1. 入力を 1 件
    const shared::hw::Event ev = _input->poll();
    const bool spinning        = _slot->phase() == Slot::Phase::Spinning;
    if (ev.kind == shared::hw::Event::Kind::HeadTap) {
        if (!spinning) {
            startGame(now);
        }
    } else if (ev.kind == shared::hw::Event::Kind::Button && ev.screen == _view->screenId()) {
        if (spinning) {
            _slot->stopReel(ev.index);
        } else {
            startGame(now);
        }
    }

    // 2. ゲームを進め、リーチと結果の演出
    const Slot::Phase before = _slot->phase();
    _slot->advance(now);
    if (_slot->reach() && !_reach_active) {
        _reach_active = true;
        _lights->setReach(true);
        _audio->play(_voice_reach);
        mclog::tagInfo(getAppInfo().name, "reach");
    } else if (!_slot->reach() && _reach_active) {
        _reach_active = false;
        _lights->setReach(false);
    }
    if (before == Slot::Phase::Spinning && _slot->phase() == Slot::Phase::Result) {
        _lights->off();
        mclog::tagInfo(getAppInfo().name, "result: {} (symbol {})", _slot->win() ? "win" : "lose", _slot->winSymbol());
    }

    // 3. LED と画面
    _lights->update(now);
    _view->render(*_slot);
    {
        LvglLockGuard lock;
        view::update_home_indicator();
    }

    // 4. ホームボタンが押されたら閉じる
    if (_close_requested) {
        _close_requested = false;
        close();
    }
}

void AppRoulette::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    // 消灯、音声と入力を止める、ホームインジケータと画面を壊す。首・カメラ・Wi-Fi は触らない。
    _lights->off();
    _input->end();
    _audio->end();
    {
        LvglLockGuard lock;
        view::destroy_home_indicator();
    }
    _view->end();

    _slot.reset();
    _view.reset();
    _lights.reset();
    _audio.reset();
    _input.reset();
}
