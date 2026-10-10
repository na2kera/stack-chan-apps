// edge_config_test のフィクスチャ (config_local.h の書き方の 1 つ)。鍵は見本の値。
#pragma once
// 旧形式の EDGE_HOST が残っていても、EDGE_BASE_URL があれば通る。
#include <cstdint>
namespace photobooth::config {
constexpr const char* EDGE_HOST       = "192.168.0.1";
constexpr uint16_t EDGE_PORT          = 8765;
constexpr const char* EDGE_BASE_URL   = "http://192.168.0.167:8765";
constexpr const char* DEVICE_ID       = "stackchan-01";
constexpr const char* EDGE_SHARED_KEY = "change-me";
}  // namespace photobooth::config
