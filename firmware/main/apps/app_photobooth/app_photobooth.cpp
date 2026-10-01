/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_photobooth.h"

#include <hal/hal.h>
#include <mooncake_log.h>

#include "assets/pb_assets.h"
#include "flow/flow.h"
#include "hw/audio.h"
#include "hw/camera.h"
#include "hw/head.h"
#include "hw/input.h"
#include "view/view.h"

using namespace mooncake;

AppPhotobooth::AppPhotobooth()
{
    // ランチャーに出る名前とアイコン (アイコンは assets パーティションではなく C 配列)
    setAppInfo().name = "Photobooth";
    setAppInfo().icon = (void*)&icon_photobooth;
    // ランチャーの背景色
    static uint32_t theme_color = 0xFFCC33;
    setAppInfo().userData       = (void*)&theme_color;
}

AppPhotobooth::~AppPhotobooth() = default;

void AppPhotobooth::onCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppPhotobooth::onOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");
    const uint32_t now = GetHAL().millis();

    _input  = std::make_unique<photobooth::hw::Input>();
    _audio  = std::make_unique<photobooth::hw::Audio>();
    _head   = std::make_unique<photobooth::hw::Head>();
    _camera = std::make_unique<photobooth::hw::Camera>();
    _view   = std::make_unique<photobooth::view::View>(*_input);
    _flow   = std::make_unique<photobooth::Flow>(*_camera, *_head, *_audio, *_view);

    // 初期化順は独立ファーム版 (device/src/main.cpp) と同じ: 入力 → 音声 → 首 → カメラ → 状態機械。
    _input->begin();
    if (!_audio->begin()) {
        mclog::tagWarn(getAppInfo().name, "audio unavailable; continue without voice");
    }
    _head->begin(now);
    const bool camera_ok = _camera->begin();
    const bool view_ok   = _view->begin();
    if (!view_ok) {
        mclog::tagWarn(getAppInfo().name, "preview buffer unavailable");
    }
    _flow->begin(now, camera_ok, view_ok);
}

void AppPhotobooth::onRunning()
{
    // 1 tick = 1 回の update()。ブロックしない (待ちは hw/ のタスク側でしている)。
    _flow->update(_input->poll(), GetHAL().millis());
    if (_flow->exitRequested()) {
        close();
    }
}

void AppPhotobooth::onClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");
    const uint32_t now = GetHAL().millis();

    // 逆順に後始末する。首は neutral へ戻す指示を出し、トルクの扱いを純正の既定に戻す。
    _flow->end(now);
    _input->end();
    _camera->end();
    _audio->end();
    _head->end(now);
    _view->end();

    _flow.reset();
    _view.reset();
    _camera.reset();
    _head.reset();
    _audio.reset();
    _input.reset();
}
