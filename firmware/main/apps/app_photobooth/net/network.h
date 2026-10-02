/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// Wi-Fi の起動と接続状態 (docs/design/fw-app-step2.md §3 「Wi-Fi 起動」)。
//
// 接続は純正の GetHAL().startNetwork() に任せる (NVS の Wi-Fi 設定を使う。app_app_center と同じ)。
// startNetwork() は繋がるまで戻らないので、専用のタスクで呼ぶ。このタスクはアプリを閉じても
// 止められない (純正側でブロックしている) ので、アプリの寿命と切り離してファイルスコープで持つ。
// ここからは切断しない (純正の他のアプリが Wi-Fi を使う)。
#pragma once

#include <cstddef>
#include <cstdint>

namespace photobooth::net {

class Network {
public:
    enum class Status : uint8_t {
        NotConfigured,  // NVS に SSID が無い (SETUP で設定が要る)。startNetwork は呼ばない
        Disconnected,   // 設定はあるが繋がっていない (純正の自動再接続待ちを含む)
        Connecting,     // startNetwork が接続を待っている
        ConfigMode,     // 純正が Wi-Fi 設定モード (AP) に入っている
        Connected,
    };

    // まだ繋がっていなければ、接続タスクを起動する (二重には起動しない)。
    // SSID が未設定なら何もしない (純正の startNetwork は未設定だと設定モード = AP に入るため)。
    static void ensureStarted();

    static Status status();
    static bool connected();

    // 診断画面用。繋がっていなければ ssid / ip は ""、rssi は 0。
    static void info(char* ssid, size_t ssid_len, char* ip, size_t ip_len, int& rssi);
};

}  // namespace photobooth::net
