/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// Wi-Fi の状態の問い合わせ (docs/design/fw-app-step2.md §3 「Wi-Fi 起動」)。
//
// 接続そのものは純正の GetHAL().startNetwork() に任せる (NVS の Wi-Fi 設定を使う)。呼ぶのは
// app_photobooth.cpp の onOpen だけで、純正の App Center と同じく同期で呼ぶ (専用タスクは作らない。
// 純正の他のアプリと同時に startNetwork() を呼ぶと、ボードのネットワークコールバックを取り合うため)。
// ここは状態を読むだけで、接続も切断もしない。
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::net {

class Network {
public:
    enum class Status : uint8_t {
        NotConfigured,  // NVS に SSID が無い (SETUP で設定が要る)
        Disconnected,   // 設定はあるが繋がっていない (純正の自動再接続待ちを含む)
        ConfigMode,     // 純正が Wi-Fi 設定モード (AP) に入っている
        Connected,
    };

    // NVS に Wi-Fi の設定 (SSID) があるか。無いときは startNetwork() を呼ばない
    // (純正の startNetwork() は未設定だと設定モード = AP に入るため)。
    static bool hasCredentials();

    // 今の状態。Flow (毎 tick) と net タスクの両方から呼ばれるので、結果を 200 ms 使い回す。
    static Status status();

    // 診断画面用。繋がっていなければ ssid / ip は ""、rssi は 0。
    static void info(char* ssid, size_t ssid_len, char* ip, size_t ip_len, int& rssi);
};

}  // namespace photobooth::net
