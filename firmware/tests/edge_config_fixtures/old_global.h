// edge_config_test のフィクスチャ (config_local.h の書き方の 1 つ)。鍵は見本の値。
#pragma once
// 旧形式をグローバル名前空間に書いた (ビルドが止まること)。
#include <cstdint>
constexpr const char* EDGE_HOST = "192.168.0.167";
constexpr uint16_t EDGE_PORT = 8765;
constexpr const char* DEVICE_ID = "stackchan-01";
constexpr const char* EDGE_SHARED_KEY = "change-me";
