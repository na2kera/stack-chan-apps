/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "network.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mooncake_log.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>

namespace photobooth::net {

namespace {

constexpr const char* kTag = "PB-Net";
// startNetwork() は Wi-Fi ドライバの初期化 (esp_wifi_init, netif, NVS) まで行う。
// 純正はメインタスクから呼んでいるので、同程度のスタックを用意する。
constexpr uint32_t kTaskStack   = 8192;
constexpr UBaseType_t kTaskPrio = 2;

// 接続タスクが startNetwork() の中にいる間 true。アプリを開き直しても引き継ぐ。
std::atomic<bool> g_starting{false};

// connected() の結果の使い回し (どのスレッドからでも読めるよう atomic。多少ずれても害は無い)。
constexpr uint32_t kConnectedCacheMs = 200;
std::atomic<bool> g_connected{false};
std::atomic<bool> g_connected_valid{false};
std::atomic<uint32_t> g_connected_checked_ms{0};

bool hasSsid()
{
    return !SsidManager::GetInstance().GetSsidList().empty();
}

void startTask(void*)
{
    mclog::tagInfo(kTag, "wifi: waiting for stock startNetwork()");
    const uint32_t t0 = GetHAL().millis();
    // 進み具合は純正 (WifiBoard) が自分でログに出す。設定モードの案内 (AP の URL を含む) は
    // ここでは出さないので、onLog は渡さない。画面には何も描かない (LVGL を触らない)。
    GetHAL().startNetwork(nullptr);
    mclog::tagInfo(kTag, "wifi: startNetwork() returned after {} ms", GetHAL().millis() - t0);
    g_starting = false;
    vTaskDelete(nullptr);
}

}  // namespace

void Network::ensureStarted()
{
    if (connected()) {
        return;
    }
    if (!hasSsid()) {
        mclog::tagWarn(kTag, "wifi: no SSID in NVS; not starting (configure Wi-Fi in SETUP)");
        return;
    }
    if (g_starting.exchange(true)) {
        return;  // 前に起動したタスクがまだ待っている
    }
    if (xTaskCreate(startTask, "pb_wifi", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        mclog::tagError(kTag, "wifi: failed to create start task");
        g_starting = false;
    }
}

bool Network::connected()
{
    // Flow (毎 tick) と net タスク (20 ms ごと) の両方から呼ばれる。getWifiStatus() は毎回 RSSI まで
    // 問い合わせるので、結果を kConnectedCacheMs だけ使い回す (切断に気づくのがその分遅れるだけ)。
    const uint32_t now = GetHAL().millis();
    if (g_connected_valid.load() && now - g_connected_checked_ms.load() < kConnectedCacheMs) {
        return g_connected.load();
    }
    // getWifiStatus() は「設定モードでなく、かつ接続済み」のとき None 以外を返す。
    const bool up = GetHAL().getWifiStatus() != WifiStatus::None;
    g_connected            = up;
    g_connected_checked_ms = now;
    g_connected_valid      = true;
    return up;
}

Network::Status Network::status()
{
    if (connected()) {
        return Status::Connected;
    }
    if (WifiManager::GetInstance().IsConfigMode()) {
        return Status::ConfigMode;
    }
    if (g_starting.load()) {
        return Status::Connecting;
    }
    return hasSsid() ? Status::Disconnected : Status::NotConfigured;
}

void Network::info(char* ssid, size_t ssid_len, char* ip, size_t ip_len, int& rssi)
{
    auto& wifi = WifiManager::GetInstance();
    if (!connected()) {
        snprintf(ssid, ssid_len, "%s", "");
        snprintf(ip, ip_len, "%s", "");
        rssi = 0;
        return;
    }
    snprintf(ssid, ssid_len, "%s", wifi.GetSsid().c_str());
    snprintf(ip, ip_len, "%s", wifi.GetIpAddress().c_str());
    rssi = wifi.GetRssi();
}

}  // namespace photobooth::net
