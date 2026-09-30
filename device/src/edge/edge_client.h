// edge (PC) との通信の抽象インターフェース (docs/design/step2b-device-edge.md §4.1)。
// イベント名は docs/spec.md §8、HTTP の契約は docs/protocol.md と一致させる。
//
// 実装 (net::HttpEdgeClient) は通信を別タスクで行うので、sessionStart などは
// 「依頼して即 return」、結果は poll*() で受け取る。App は loop() (core 1) からだけ呼ぶ。
#pragma once

#include <esp_camera.h>

#include <cstddef>
#include <cstdint>

#include "app/session.h"

namespace edge {

// frame_result の hint (protocol.md)。
enum class Hint : uint8_t { None, Closer, TooMany };

// frame の X-Phase。
enum class Phase : uint8_t { Compose, Capture };

// frame_result (spec §8, protocol.md)。
struct FrameResult {
  bool valid = false;
  uint32_t frame_id = 0;
  bool dropped = false;
  uint8_t face_count = 0;
  uint8_t target_face_count = 0;
  bool all_in_frame = false;
  bool all_eyes_open = false;
  bool all_smiling = false;
  int servo_dx = 0;  // 1/10 度。device は head.nudge() でさらにクランプする
  int servo_dy = 0;
  Hint hint = Hint::None;
  bool accepted = false;
  uint16_t latency_ms = 0;  // edge 側の処理時間
};

// photo_ready (GET …/photo)。
struct PhotoInfo {
  enum class Status : uint8_t { Pending, Ready, Error };
  Status status = Status::Pending;
  char photo_url[256] = {};
  char share_url[256] = {};
  char expires_at[32] = {};  // ISO 8601 (+09:00)
  char reason[32] = {};      // status == Error のとき (例 "upload_failed")
};

// 診断画面 (DIAG) に出す接続状態。鍵は含めない。
struct Diagnostics {
  bool enabled = false;       // edge 通信ありのビルドか (NullEdge は false)
  bool wifi_connected = false;
  bool wifi_connecting = false;
  bool online = false;        // edge から直近に 2xx を受けている
  char ssid[33] = {};
  char ip[16] = {};
  int rssi = 0;               // dBm。wifi_connected のときだけ有効
  char edge_host[64] = {};
  uint16_t edge_port = 0;
  char last_error[64] = {};
};

class EdgeClient {
 public:
  virtual ~EdgeClient() = default;

  // 直近に hello か任意のリクエストで 2xx を受けていれば true。
  virtual bool isOnline() = 0;

  // 非同期 (コマンドキュー)。以前のセッションの結果・保留フレームは捨てる。
  virtual void sessionStart(const app::Session&) = 0;

  // fb をフレームスロットへコピーして即 return する (fb は直後に呼び出し側がドライバへ返す)。
  // 送信中なら次に送るフレームを上書きする (newest wins)。戻り値はコピーできたか。
  virtual bool offerFrame(const app::Session&, const camera_fb_t& fb, int servo_x, int servo_y,
                          Phase phase) = 0;

  // 最新の frame_result を 1 回だけ返す。新しい結果が無ければ false。
  virtual bool pollResult(FrameResult& out) = 0;

  // 非同期。結果は pollTimeout()。
  virtual void sessionTimeout(const app::Session&) = 0;
  // session_timeout の結果が出たら true (1 回だけ)。ok=true は 200 で JSON を読めたときだけで、
  // そのとき has_candidate が候補の有無。ok=false は通信失敗・200 以外・JSON 不正
  // (edge 再起動後の 404 など。理由は lastError())。「候補なし」とは区別する。
  virtual bool pollTimeout(bool& ok, bool& has_candidate) = 0;

  // 候補 JPEG (GET …/candidate) を同期で取る (最大 EDGE_TIMEOUT_MS 程度)。
  // 成功なら PSRAM に確保したバッファを返す。呼び出し側が heap_caps_free() する。
  virtual bool fetchCandidate(const app::Session&, uint8_t*& jpeg, size_t& len) = 0;

  // 非同期。save は送信に失敗したら UPLOAD_RETRY 回まで送り直し、成功したら photo をポーリングする。
  virtual void reviewDecision(const app::Session&, bool save) = 0;
  // 写真の準備が終わった (ready / error) ら true (1 回だけ)。pending の間は false。
  virtual bool pollPhotoReady(PhotoInfo& out) = 0;

  virtual void sessionCancel(const app::Session&) = 0;

  // 診断画面用。最後の通信エラーの短い説明 (鍵・URL は含めない)。無ければ ""。
  virtual const char* lastError() = 0;
  virtual void diagnostics(Diagnostics& out) = 0;
  // Wi-Fi を切断して繋ぎ直し、すぐ hello する (診断画面の「再接続」)。
  virtual void reconnect() = 0;
};

}  // namespace edge
