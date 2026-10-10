/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// app_photobooth の net/edge_config.h が config_local.h の各書き方 (photobooth::config 内の constexpr /
// グローバルの constexpr / #define) を受け付け、旧形式 (EDGE_HOST / EDGE_PORT) と接続先なしでビルドを
// 止めることを確かめる (docs/design/step6-cloud-device.md §3.2「接続先の設定」、試験 11)。
//
// CMakeLists.txt がフィクスチャ (edge_config_fixtures/*.h) ごとに config.h / net/edge_config.h と並べて
// config_local.h を置いたディレクトリを作り、そこを include パスにしてこのファイルをコンパイルする。
// 通るはずのものは実行して値を比べ、止まるはずのものは -fsyntax-only のエラー文言を ctest が確かめる。
#include "net/edge_config.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#ifndef EXPECTED_BASE_URL
#define EXPECTED_BASE_URL ""
#endif

int main()
{
    static_assert(PHOTOBOOTH_EDGE_ENABLED == 1, "config_local.h が見つかっていない");
    namespace local = photobooth::config::local;
    if (std::strcmp(local::kEdgeBaseUrl, EXPECTED_BASE_URL) != 0) {
        std::cerr << "kEdgeBaseUrl: expected \"" << EXPECTED_BASE_URL << "\", got \"" << local::kEdgeBaseUrl
                  << "\"\n";
        return 1;
    }
    if (std::strcmp(local::kDeviceId, "stackchan-01") != 0 || std::strcmp(local::kSharedKey, "change-me") != 0) {
        std::cerr << "kDeviceId / kSharedKey mismatch\n";
        return 1;
    }
    std::cout << "edge_config_test (" << FIXTURE_NAME << "): ok\n";
    return 0;
}
