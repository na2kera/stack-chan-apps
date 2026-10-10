/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 写真撮影アプリ (docs/design/fw-app-step1.md, fw-app-step2.md)。純正ランチャーから開く Mooncake アプリ。
#pragma once
#include <mooncake.h>

#include <memory>

namespace shared::hw {
class Audio;
class Input;
}  // namespace shared::hw

namespace photobooth {
class Flow;
namespace hw {
class Camera;
class Clips;
class Head;
}  // namespace hw
namespace net {
class EdgeClient;
}
namespace view {
class View;
}
}  // namespace photobooth

class AppPhotobooth : public mooncake::AppAbility {
public:
    AppPhotobooth();
    ~AppPhotobooth();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // onOpen で作り、onClose で壊す (アプリを開いていない間は PSRAM もタスクも使わない)。
    std::unique_ptr<shared::hw::Input> _input;
    std::unique_ptr<shared::hw::Audio> _audio;
    std::unique_ptr<photobooth::hw::Clips> _clips;
    std::unique_ptr<photobooth::hw::Head> _head;
    std::unique_ptr<photobooth::hw::Camera> _camera;
    std::unique_ptr<photobooth::view::View> _view;
    std::unique_ptr<photobooth::net::EdgeClient> _edge;  // edge 無効ビルドでは何もしないスタブ
    std::unique_ptr<photobooth::Flow> _flow;
};
