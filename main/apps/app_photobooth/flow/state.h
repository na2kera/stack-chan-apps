/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 撮影フローの状態 (docs/spec.md §4 の表と 1:1)。device/src/app/state.h の移植。
#pragma once

#include <cstdint>

#include "../view/strings.h"

namespace photobooth {

enum class State : uint8_t {
    Idle,
    Announce,
    Compose,
    Capture,
    Review,
    Uploading,
    PhotoQr,
    XQr,
    Error,
};

// ログ用の名前 (spec §4 の表記)。
inline const char* stateName(State s)
{
    switch (s) {
        case State::Idle:
            return "IDLE";
        case State::Announce:
            return "ANNOUNCE";
        case State::Compose:
            return "COMPOSE";
        case State::Capture:
            return "CAPTURE";
        case State::Review:
            return "REVIEW";
        case State::Uploading:
            return "UPLOADING";
        case State::PhotoQr:
            return "PHOTO_QR";
        case State::XQr:
            return "X_QR";
        case State::Error:
            return "ERROR";
    }
    return "?";
}

// 画面のタイトル帯に出す日本語の状態名。
inline const char* stateTitle(State s)
{
    switch (s) {
        case State::Idle:
            return str::kTitleIdle;
        case State::Announce:
            return str::kTitleAnnounce;
        case State::Compose:
            return str::kTitleCompose;
        case State::Capture:
            return str::kTitleCapture;
        case State::Review:
            return str::kTitleReview;
        case State::Uploading:
            return str::kTitleUploading;
        case State::PhotoQr:
            return str::kTitlePhotoQr;
        case State::XQr:
            return str::kTitleXQr;
        case State::Error:
            return str::kTitleError;
    }
    return "";
}

}  // namespace photobooth
