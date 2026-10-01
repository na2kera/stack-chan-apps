/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 写真撮影アプリ (docs/design/fw-app-step1.md)。純正ランチャーから開く Mooncake アプリ。
#pragma once
#include <mooncake.h>

#include <memory>

namespace photobooth {
class Flow;
namespace hw {
class Audio;
class Camera;
class Head;
class Input;
}  // namespace hw
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
    std::unique_ptr<photobooth::hw::Input> _input;
    std::unique_ptr<photobooth::hw::Audio> _audio;
    std::unique_ptr<photobooth::hw::Head> _head;
    std::unique_ptr<photobooth::hw::Camera> _camera;
    std::unique_ptr<photobooth::view::View> _view;
    std::unique_ptr<photobooth::Flow> _flow;
};
