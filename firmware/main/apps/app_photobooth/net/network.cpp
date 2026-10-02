/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "network.h"

#include <hal/hal.h>
#include <ssid_manager.h>
#include <wifi_manager.h>

#include <atomic>
#include <cstdio>

namespace photobooth::net {

namespace {

// status() の結果の使い回し (どのスレッドからでも読めるよう atomic。多少ずれても害は無い)。
constexpr uint32_t kStatusCacheMs = 200;
std::atomic<uint8_t> g_status{static_cast<uint8_t>(Network::Status::Disconnected)};
std::atomic<bool> g_status_valid{false};
std::atomic<uint32_t> g_status_checked_ms{0};

Network::Status readStatus()
{
    // getWifiStatus() は「設定モードでなく、かつ接続済み」のとき None 以外を返す。
    if (GetHAL().getWifiStatus() != WifiStatus::None) {
        return Network::Status::Connected;
    }
    if (WifiManager::GetInstance().IsConfigMode()) {
        return Network::Status::ConfigMode;
    }
    return Network::hasCredentials() ? Network::Status::Disconnected : Network::Status::NotConfigured;
}

}  // namespace

bool Network::hasCredentials()
{
    return !SsidManager::GetInstance().GetSsidList().empty();
}

Network::Status Network::status()
{
    // getWifiStatus() は毎回 RSSI まで問い合わせるので、kStatusCacheMs だけ使い回す
    // (切断に気づくのがその分遅れるだけ)。
    const uint32_t now = GetHAL().millis();
    if (g_status_valid.load() && now - g_status_checked_ms.load() < kStatusCacheMs) {
        return static_cast<Status>(g_status.load());
    }
    return statusNow();
}

Network::Status Network::statusNow()
{
    const Status s      = readStatus();
    g_status            = static_cast<uint8_t>(s);
    g_status_checked_ms = GetHAL().millis();
    g_status_valid      = true;
    return s;
}

void Network::info(char* ssid, size_t ssid_len, char* ip, size_t ip_len, int& rssi)
{
    auto& wifi = WifiManager::GetInstance();
    if (status() != Status::Connected) {
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
