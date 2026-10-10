// edge_config_test のフィクスチャ (config_local.h の書き方の 1 つ)。鍵は見本の値。
#pragma once
#include <cstdint>
namespace photobooth::config {
constexpr const char* EDGE_BASE_URL   = "http://192.168.0.167:8765";
constexpr const char* DEVICE_ID       = "stackchan-01";
constexpr const char* EDGE_SHARED_KEY = "change-me";
}  // namespace photobooth::config
