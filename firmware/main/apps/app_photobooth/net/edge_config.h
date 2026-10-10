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

#include <cstddef>
#include <cstdint>

#include "../config.h"

#if PHOTOBOOTH_EDGE_ENABLED
#include "../config_local.h"

// ---- 旧形式 (EDGE_HOST / EDGE_PORT) の検出 ----
// #define で書かれていればプリプロセッサで分かる。
#if defined(EDGE_HOST) && !defined(EDGE_BASE_URL)
#error "config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました (firmware/README.md「edge と繋ぐ」)。EDGE_BASE_URL = \"http://<旧 EDGE_HOST>:<旧 EDGE_PORT>\" と書き換えてください"
#endif

// constexpr で書かれた名前はプリプロセッサから見えないので、名前の有無を型で調べる。
// グローバル名前空間に同じ名前のクラス (目印) を置く。C++ では同じ有効範囲の変数がクラス名を隠すので、
//   - photobooth::config の中の定義 (見本の書き方) はそちらが先に見つかる。
//   - グローバル名前空間の constexpr は、目印のクラスを隠して変数が見つかる。
//   - どちらも無ければ目印のクラス (型) が見つかる。
// sizeof は型にも式にも使えるので、目印の大きさかどうかで有無が分かる。#define なら目印を置かない。
namespace photobooth::config::local::probe {
inline constexpr size_t kMarkerSize = 4093;  // 文字列のポインタ・配列と重ならない大きさ
}  // namespace photobooth::config::local::probe
#ifndef EDGE_BASE_URL
struct EDGE_BASE_URL {
    char marker[photobooth::config::local::probe::kMarkerSize];
};
#endif
#ifndef EDGE_HOST
struct EDGE_HOST {
    char marker[photobooth::config::local::probe::kMarkerSize];
};
#endif

namespace photobooth::config::local {

namespace probe {

inline constexpr bool kHasBaseUrl = sizeof(EDGE_BASE_URL) != kMarkerSize;
inline constexpr bool kHasOldHost = sizeof(EDGE_HOST) != kMarkerSize;

// 値の取り出し。EDGE_BASE_URL が型 (目印) なら 1 つ目、変数なら 2 つ目が選ばれる。
template <class T>
constexpr const char* baseUrlOf()
{
    return "";
}
template <const auto& V>
constexpr const char* baseUrlOf()
{
    return V;
}
#ifdef EDGE_BASE_URL
inline constexpr const char* kBaseUrl = EDGE_BASE_URL;  // #define (文字列リテラル)
#else
inline constexpr const char* kBaseUrl = baseUrlOf<EDGE_BASE_URL>();
#endif

}  // namespace probe

static_assert(probe::kHasBaseUrl || !probe::kHasOldHost,
              "config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました (firmware/README.md「edge と繋ぐ」)。"
              "EDGE_BASE_URL = \"http://<旧 EDGE_HOST>:<旧 EDGE_PORT>\" と書き換えてください");
static_assert(probe::kHasBaseUrl || probe::kHasOldHost,  // 旧形式なら上の案内だけを出す
              "config_local.h: EDGE_BASE_URL がありません (config_local.example.h を見本に書いてください)");

// config_local.h の書き方 (photobooth::config 内の constexpr / グローバルの constexpr / #define) の
// どれでも受けられるよう、非修飾名で引き直す。EDGE_BASE_URL の書式は net/edge_url で確かめる (不正なら begin() が失敗する)。
inline constexpr const char* kEdgeBaseUrl = probe::kBaseUrl;
inline constexpr const char* kDeviceId    = DEVICE_ID;
inline constexpr const char* kSharedKey   = EDGE_SHARED_KEY;

}  // namespace photobooth::config::local
#endif
