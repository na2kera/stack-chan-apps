/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// 撮影フローの状態 (docs/spec.md §4 の表 + 診断画面 DIAG)。device/src/app/state.h の移植。
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
    Diag,  // 接続診断 (edge に繋がらないときに待機画面のタッチで開く。fw-app-step2.md §4)
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
        case State::Diag:
            return "DIAG";
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
        case State::Diag:
            return str::kTitleDiag;
    }
    return "";
}

}  // namespace photobooth
