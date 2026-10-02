/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge の接続先と共有鍵 (config_local.h) を読む唯一の場所。net/http_edge_client.cpp だけが include する。
// 値はログにも画面にも出さない (host:port だけは診断画面に出す)。
#pragma once

#include <cstdint>

#include "../config.h"

#if PHOTOBOOTH_EDGE_ENABLED
#include "../config_local.h"

namespace photobooth::config::local {

// config_local.h の書き方 (photobooth::config 内の constexpr / グローバルの constexpr / #define) の
// どれでも受けられるよう、非修飾名で引き直す。
inline constexpr const char* kEdgeHost  = EDGE_HOST;
inline constexpr uint16_t kEdgePort     = EDGE_PORT;
inline constexpr const char* kDeviceId  = DEVICE_ID;
inline constexpr const char* kSharedKey = EDGE_SHARED_KEY;

}  // namespace photobooth::config::local
#endif
