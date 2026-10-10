/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth のローカル設定の見本 (docs/design/fw-app-step2.md §2)。
//
//   cp config_local.example.h config_local.h   してから値を書き換える。
//
// config_local.h は .gitignore 済み (共有鍵を Git に入れない)。このファイルが無い、または
// `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` でビルドすると edge 通信なし (ステップ1 と同じ固定 URL) になる。
// Wi-Fi の SSID / パスワードはここに書かない (純正の Wi-Fi 設定 = NVS を使う)。
#pragma once

namespace photobooth::config {

// edge の接続先 URL。書式は scheme "://" host [":" port] ["/"] (path・query・userinfo は書けない)。
//   - LAN の PC の edge: "http://<PC の IP>:8765"。IP で書く (`.local` 名は未対応)。Mac なら `ipconfig getifaddr en0`。
//     DHCP で変わることがあるので、繋がらないときは診断画面の「接続先」の行と見比べる。
//   - クラウドの edge: "https://stackchan-edge.<account>.workers.dev"。https はホスト名で書く (IP は不可)。
//     証明書は常に検証する (ESP-IDF の証明書バンドル)。
// 以前の EDGE_HOST / EDGE_PORT は使えない。EDGE_HOST = "192.168.1.10", EDGE_PORT = 8765 だったなら
// EDGE_BASE_URL = "http://192.168.1.10:8765" と書き換える (旧形式のままだとビルドが止まる)。
constexpr const char* EDGE_BASE_URL = "http://192.168.1.10:8765";
// edge の config.toml [auth] の device_id / device_key (または環境変数 EDGE_DEVICE_KEY) と揃える。
// 違うと edge が 401 を返し、診断画面に「認証エラー」と出る。
constexpr const char* DEVICE_ID       = "stackchan-01";
constexpr const char* EDGE_SHARED_KEY = "change-me";

}  // namespace photobooth::config
