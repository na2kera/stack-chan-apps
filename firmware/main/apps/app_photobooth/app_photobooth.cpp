/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_photobooth.h"

#include <hal/hal.h>
#include <mooncake_log.h>

#include "assets/pb_assets.h"
#include "config.h"
#include "flow/flow.h"
#include "hw/audio.h"
#include "hw/camera.h"
#include "hw/head.h"
#include "hw/input.h"
#include "net/http_edge_client.h"
#include "net/network.h"
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

    _input  = std::make_unique<photobooth::hw::Input>();
    _audio  = std::make_unique<photobooth::hw::Audio>();
    _head   = std::make_unique<photobooth::hw::Head>();
    _camera = std::make_unique<photobooth::hw::Camera>();
    _view   = std::make_unique<photobooth::view::View>(*_input);
    _edge   = photobooth::net::createEdgeClient();
    _flow   = std::make_unique<photobooth::Flow>(*_camera, *_head, *_audio, *_view, *_edge);

    // edge と繋ぐビルドなら、先に Wi-Fi を繋ぐ。純正の App Center と同じく startNetwork() を同期で呼び、
    // 繋がるまで「Wi-Fi接続中」の画面を出す (進み具合の文言は純正の onLog をそのまま出す)。
    //   - NVS に SSID が無ければ呼ばない (純正は未設定だと設定モード = AP に入る)。「PC未接続」で始まる。
    //   - SSID はあるが繋がらないときは純正の動作になる (約 60 秒で設定モードに入り、繋がるまで戻らない)。
    //   - 首・カメラ・音声はこの後で初期化する (待っている間はトルクも出力も入れない)。
    if (photobooth::config::EDGE_ENABLED) {
        using photobooth::net::Network;
        if (Network::status() == Network::Status::Connected) {
            mclog::tagInfo(getAppInfo().name, "wifi already connected");
        } else if (!Network::hasCredentials()) {
            mclog::tagWarn(getAppInfo().name, "wifi: no SSID in NVS; start offline (configure Wi-Fi in SETUP)");
        } else {
            _view->showWifiConnecting();
            const uint32_t t0 = GetHAL().millis();
            GetHAL().startNetwork([this](std::string_view msg) { _view->setWifiMessage(msg); });
            mclog::tagInfo(getAppInfo().name, "wifi connected after {} ms", GetHAL().millis() - t0);
        }
    }
    const uint32_t now = GetHAL().millis();

    // 初期化順は独立ファーム版 (device/src/main.cpp) と同じ: 入力 → 音声 → 首 → カメラ → 通信 → 状態機械。
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
    // net タスクの起動。ブロックしない (edge の接続状態は待機画面の右下に出る)。
    if (!_edge->begin()) {
        mclog::tagWarn(getAppInfo().name, "edge client unavailable: {}", _edge->lastError());
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
    // net タスクを止める (1 秒で止まらなければ切り離す)。Wi-Fi は切らない (純正の他のアプリが使う)。
    _edge->end();
    _input->end();
    _camera->end();
    _audio->end();
    _head->end(now);
    _view->end();

    _flow.reset();
    _edge.reset();
    _view.reset();
    _camera.reset();
    _head.reset();
    _audio.reset();
    _input.reset();
}
