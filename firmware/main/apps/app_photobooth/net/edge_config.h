/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge の接続先と共有鍵 (config_local.h) を読む唯一の場所。net/http_edge_client.cpp だけが include する。
// 値はログにも画面にも出さない (scheme / host / port だけは診断画面に出す)。
//
// 接続先は EDGE_BASE_URL (例 "https://stackchan-edge.xxx.workers.dev" / "http://192.168.0.167:8765")。
// 以前の EDGE_HOST / EDGE_PORT は受け付けない (意図的な破壊変更。docs/design/step6-cloud-device.md §3.2)。
// 旧形式の config_local.h のままビルドすると、移行手順を示してビルドを止める。
#pragma once

#include <cstdint>
#include <type_traits>

#include "../config.h"

#if PHOTOBOOTH_EDGE_ENABLED
#include "../config_local.h"

// ---- 旧形式 (EDGE_HOST / EDGE_PORT) の検出 ----
// #define で書かれていればプリプロセッサで分かる。
#if defined(EDGE_HOST) && !defined(EDGE_BASE_URL)
#error "config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました (firmware/README.md「edge と繋ぐ」)。EDGE_BASE_URL = \"http://<旧 EDGE_HOST>:<旧 EDGE_PORT>\" と書き換えてください"
#endif

// constexpr で書かれた名前はプリプロセッサから見えないので、名前の有無を型で調べる。
// 下の「代わりの名前」は using 指令でグローバル名前空間に見えるので、photobooth::config (見本の書き方) の
// 定義があればそちらが先に見つかり、無ければ代わりの名前 (Missing 型) が見つかる。
// (このため config_local.h の名前は photobooth::config の中か #define で書く。グローバル名前空間に
//  constexpr で書くと、代わりの名前とあいまいになってビルドが止まる。)
namespace photobooth_edge_config_fallback {
struct Missing {};
#ifndef EDGE_BASE_URL
inline constexpr Missing EDGE_BASE_URL{};
#endif
#ifndef EDGE_HOST
inline constexpr Missing EDGE_HOST{};
#endif
}  // namespace photobooth_edge_config_fallback

namespace photobooth::config::local {

namespace probe {
using namespace ::photobooth_edge_config_fallback;
using ::photobooth_edge_config_fallback::Missing;

inline constexpr bool kHasBaseUrl = !std::is_same_v<std::decay_t<decltype(EDGE_BASE_URL)>, Missing>;
inline constexpr bool kHasOldHost = !std::is_same_v<std::decay_t<decltype(EDGE_HOST)>, Missing>;

constexpr const char* baseUrlOr(const char* v)
{
    return v;
}
constexpr const char* baseUrlOr(Missing)
{
    return "";
}
inline constexpr const char* kBaseUrl = baseUrlOr(EDGE_BASE_URL);
}  // namespace probe

static_assert(probe::kHasBaseUrl || !probe::kHasOldHost,
              "config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました (firmware/README.md「edge と繋ぐ」)。"
              "EDGE_BASE_URL = \"http://<旧 EDGE_HOST>:<旧 EDGE_PORT>\" と書き換えてください");
static_assert(probe::kHasBaseUrl || probe::kHasOldHost,  // 旧形式なら上の案内だけを出す
              "config_local.h: EDGE_BASE_URL がありません (config_local.example.h を見本に書いてください)");

// config_local.h の書き方 (photobooth::config 内の constexpr / #define) のどちらでも受けられるよう、
// 非修飾名で引き直す。EDGE_BASE_URL の書式は net/edge_url で確かめる (不正なら begin() が失敗する)。
inline constexpr const char* kEdgeBaseUrl = probe::kBaseUrl;
inline constexpr const char* kDeviceId    = DEVICE_ID;
inline constexpr const char* kSharedKey   = EDGE_SHARED_KEY;

}  // namespace photobooth::config::local
#endif
