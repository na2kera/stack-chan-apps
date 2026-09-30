// EdgeClient の HTTP 実装 (docs/design/step2b-device-edge.md §4.2, docs/protocol.md)。
//
// 通信は net タスク (core 0) 1 本だけが行う。App (loop, core 1) との受け渡しは次の 3 つ:
//   - コマンドキュー (xQueue): start / timeout / review / cancel / candidate / reconnect
//   - フレームスロット: PSRAM の 2 面。App が書く面 (write) と net タスクが送る面 (send) を
//     mutex 下のインデックス交換で入れ替える (所有権の移動。送信中の面に App は触らない)。
//     送信中に次のフレームが来たら write 面を上書きする (newest wins、キューを溜めない)。
//   - mailbox: frame_result / timeout / photo / candidate の最新 1 件 + fresh フラグ (mutex)。
// セッションの世代 (generation_) を sessionStart / sessionCancel で進め、古い世代の結果は捨てる。
//
// M5 / 表示は net タスクから呼ばない。鍵・URL・画像バイトはログに出さない。
#pragma once

#include <HTTPClient.h>
#include <NetworkClient.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "edge/edge_client.h"
#include "net/wifi_link.h"

namespace net {

class HttpEdgeClient : public edge::EdgeClient {
 public:
  explicit HttpEdgeClient(WifiLink& wifi) : wifi_(wifi) {}
  HttpEdgeClient(const HttpEdgeClient&) = delete;
  HttpEdgeClient& operator=(const HttpEdgeClient&) = delete;

  // フレームスロット (PSRAM)・mutex・キューを確保して net タスクを起動する。
  // 失敗したら false (以後 isOnline() は false のまま)。
  bool begin();

  bool isOnline() override;
  void sessionStart(const app::Session& s) override;
  bool offerFrame(const app::Session& s, const camera_fb_t& fb, int servo_x, int servo_y,
                  edge::Phase phase) override;
  bool pollResult(edge::FrameResult& out) override;
  void sessionTimeout(const app::Session& s) override;
  bool pollTimeout(bool& has_candidate) override;
  bool fetchCandidate(const app::Session& s, uint8_t*& jpeg, size_t& len) override;
  void reviewDecision(const app::Session& s, bool save) override;
  bool pollPhotoReady(edge::PhotoInfo& out) override;
  void sessionCancel(const app::Session& s) override;
  const char* lastError() override;
  void diagnostics(edge::Diagnostics& out) override;
  void reconnect() override;

  // QVGA RGB565 1 枚分。これより大きいフレームは offerFrame() で断る。
  static constexpr size_t kSlotBytes = 320 * 240 * 2;

 private:
  enum class CmdKind : uint8_t { Start, Timeout, Review, Cancel, Candidate, Reconnect };
  struct Command {
    CmdKind kind;
    bool save;           // Review
    uint32_t gen;        // 依頼時の世代
    uint32_t seq;        // Candidate の依頼番号
    uint32_t started_ms; // Start
    char sid[37];
  };
  struct FrameMeta {
    char sid[37];
    uint32_t gen;
    uint32_t frame_id;
    uint32_t capture_ms;
    int servo_x;
    int servo_y;
    uint16_t width;
    uint16_t height;
    size_t len;
    edge::Phase phase;
  };
  // 1 回の HTTP のやりとりの結果。status < 0 は HTTPClient のエラー (通信失敗)。
  struct Reply {
    int status = 0;
    char error_code[40] = {};  // エラー応答の {"error": ...}
  };

  static void taskEntry(void* arg);
  void taskLoop();

  // ---- net タスクだけが呼ぶ ----
  void handleCommand(const Command& c);
  bool takeFrame(FrameMeta& meta);
  void dropPendingFrame();
  void sendFrame(const FrameMeta& meta);
  void sendHello();
  void pollPhoto(uint32_t now);
  void publishPhoto(uint32_t gen, const edge::PhotoInfo& info);
  // JSON / 空本文のリクエスト。body_out が非 null なら 2xx の本文 (max_body まで) を返す。
  // retry_transport なら通信失敗 (status < 0) のとき接続を捨てて 1 回だけ送り直す
  // (keep-alive の接続が edge 側で閉じられていた場合の対策。冪等なリクエストだけ)。
  Reply request(const char* op, const char* method, const char* path, const char* json,
                String* body_out, size_t max_body, bool retry_transport);
  Reply requestOnce(const char* method, const char* path, const char* json, String* body_out,
                    size_t max_body);
  bool fetchCandidateNow(const char* sid, uint8_t*& buf, size_t& len);
  void beginHttp(const char* path);  // 認証ヘッダまで付ける
  void finishHttp(bool ok);
  void noteResponse(const char* op, const Reply& r);
  void noteSuccess();
  void noteFailure(const char* op, const Reply& r);
  void setError(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void logStats(uint32_t now);
  bool currentGen(uint32_t gen);

  // ---- App (loop) が呼ぶ ----
  void enqueue(const Command& c);
  void clearMailboxesLocked();  // mutex_ を持って呼ぶ

  WifiLink& wifi_;
  HTTPClient http_;
  NetworkClient tcp_;

  TaskHandle_t task_ = nullptr;
  QueueHandle_t queue_ = nullptr;
  SemaphoreHandle_t mutex_ = nullptr;
  SemaphoreHandle_t cand_sem_ = nullptr;

  // ---- mutex_ で守る ----
  uint32_t generation_ = 0;
  uint8_t* slot_[2] = {nullptr, nullptr};
  int write_idx_ = 0;  // App が書く面。もう一方 (send_idx_) は net タスクの所有
  bool has_frame_ = false;
  FrameMeta pending_{};
  edge::FrameResult result_{};
  bool result_fresh_ = false;
  // accepted=true の結果は result_ とは別に持ち、App が受け取るまで消さない (sticky)。
  // 後から届いた dropped=true の応答で上書きされないようにするため。
  edge::FrameResult accepted_result_{};
  bool accepted_pending_ = false;
  // accepted を受けた世代ではフレームを送らない (sessionStart / sessionCancel で解除)。
  bool frames_stopped_ = false;
  bool timeout_fresh_ = false;
  bool timeout_has_candidate_ = false;
  edge::PhotoInfo photo_{};
  bool photo_fresh_ = false;
  uint32_t cand_wanted_seq_ = 0;  // App が待っている依頼番号 (0 = 待っていない)
  uint32_t cand_done_seq_ = 0;
  uint8_t* cand_buf_ = nullptr;
  size_t cand_len_ = 0;
  char last_error_[96] = {};

  // ---- net タスクと App の両方から読む ----
  std::atomic<uint32_t> last_ok_ms_{0};
  std::atomic<bool> ever_ok_{false};
  std::atomic<uint8_t> failures_{0};  // 連続失敗回数

  // ---- App (loop) だけが触る ----
  uint32_t cand_seq_ = 0;
  char error_copy_[96] = {};

  // ---- net タスクだけが触る ----
  int send_idx_ = 1;
  uint32_t last_request_ms_ = 0;
  bool requested_once_ = false;
  bool wifi_was_up_ = false;
  bool poll_active_ = false;  // save の後の GET …/photo ポーリング
  uint32_t poll_gen_ = 0;
  uint32_t poll_started_ms_ = 0;
  uint32_t poll_last_ms_ = 0;
  char poll_sid_[37] = {};
  // 送信 fps と往復時間 (logStats で出してリセット)
  uint32_t stats_since_ms_ = 0;
  uint32_t stats_frames_ = 0;
  uint32_t stats_failures_ = 0;
  uint32_t stats_skipped_ = 0;  // offline で送らずに捨てたフレーム
  uint32_t stats_rtt_sum_ = 0;
  uint32_t stats_rtt_max_ = 0;
  uint32_t stats_edge_sum_ = 0;
};

}  // namespace net
