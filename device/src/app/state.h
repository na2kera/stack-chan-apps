// 撮影フローの状態 (docs/spec.md §4 の表と 1:1)。
#pragma once

#include <cstdint>

namespace app {

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
inline const char* stateName(State s) {
  switch (s) {
    case State::Idle:      return "IDLE";
    case State::Announce:  return "ANNOUNCE";
    case State::Compose:   return "COMPOSE";
    case State::Capture:   return "CAPTURE";
    case State::Review:    return "REVIEW";
    case State::Uploading: return "UPLOADING";
    case State::PhotoQr:   return "PHOTO_QR";
    case State::XQr:       return "X_QR";
    case State::Error:     return "ERROR";
  }
  return "?";
}

// 画面のタイトル帯に出す日本語の状態名。
inline const char* stateTitle(State s) {
  switch (s) {
    case State::Idle:      return "待機中";
    case State::Announce:  return "撮影開始";
    case State::Compose:   return "構図あわせ";
    case State::Capture:   return "撮影中";
    case State::Review:    return "確認";
    case State::Uploading: return "準備中";
    case State::PhotoQr:   return "写真を保存";
    case State::XQr:       return "Xに投稿";
    case State::Error:     return "エラー";
  }
  return "";
}

}  // namespace app
