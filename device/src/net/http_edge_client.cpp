#include "net/http_edge_client.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include "config.h"

namespace net {

namespace {

constexpr const char* TAG = "net";

constexpr int kProtocolVersion = 1;
constexpr uint32_t kTaskStackBytes = 8192;  // design §2。高水位は logStats() で出す
constexpr UBaseType_t kTaskPriority = 3;     // loopTask (1) より上、Wi-Fi / lwIP より下
constexpr BaseType_t kTaskCore = 0;          // App の loop() は core 1
constexpr UBaseType_t kQueueDepth = 8;
// この回数続けて失敗したら offline にしてフレームを送らず、hello で復帰を待つ (design §4.2)。
constexpr uint8_t kOfflineAfterFailures = 3;
// hello は HELLO_INTERVAL_MS ごとなので、1 回の hello の往復で offline に見えないよう窓は 2 倍にする。
constexpr uint32_t kOnlineWindowMs = config::HELLO_INTERVAL_MS * 2;
constexpr uint32_t kPhotoPollIntervalMs = 500;
constexpr uint32_t kSaveRetryDelayMs = 300;
constexpr size_t kMaxJsonBody = 2048;               // frame_result / photo は 1 KB 未満
constexpr size_t kMaxCandidateBytes = 256 * 1024;   // QVGA 品質 80 の JPEG は数十 KB
constexpr uint32_t kCandidateWaitMarginMs = 500;
constexpr uint32_t kStatsIntervalMs = 5000;
constexpr uint32_t kIdleWaitMs = 20;

// HTTPClient のエラー (負値) と重ならない独自のエラー。
constexpr int kErrNoWifi = -100;
constexpr int kErrBodyTooLarge = -101;
constexpr int kErrBadJson = -102;
constexpr int kErrMismatch = -103;

static_assert(config::EDGE_TIMEOUT_MS <= 65535, "HTTPClient::setTimeout takes uint16_t");
static_assert(config::UPLOAD_RETRY >= 1, "UPLOAD_RETRY must be >= 1");

void copyStr(char* dst, size_t n, const char* src) { snprintf(dst, n, "%s", src != nullptr ? src : ""); }

edge::Hint parseHint(const char* h) {
  if (h == nullptr) return edge::Hint::None;
  if (strcmp(h, "closer") == 0) return edge::Hint::Closer;
  if (strcmp(h, "too_many") == 0) return edge::Hint::TooMany;
  return edge::Hint::None;
}

const char* phaseName(edge::Phase p) { return p == edge::Phase::Capture ? "capture" : "compose"; }

// 画面 (診断・ERROR) に出す短い説明。URL や鍵を含めない。
const char* statusText(int status) {
  switch (status) {
    case kErrNoWifi: return "Wi-Fi未接続";
    case kErrBodyTooLarge: return "応答が大きすぎます";
    case kErrBadJson: return "応答を読めません";
    case kErrMismatch: return "応答が要求と合いません";
    case HTTPC_ERROR_CONNECTION_REFUSED: return "PCに接続できません";
    case HTTPC_ERROR_SEND_HEADER_FAILED:
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return "送信に失敗しました";
    case HTTPC_ERROR_NOT_CONNECTED:
    case HTTPC_ERROR_CONNECTION_LOST: return "接続が切れました";
    case HTTPC_ERROR_READ_TIMEOUT: return "PCの応答がありません";
    case 401: return "認証エラー(IDか鍵が違う)";
    default: return status < 0 ? "通信エラー" : "";
  }
}

uint8_t clampU8(int v) { return static_cast<uint8_t>(std::min(std::max(v, 0), 255)); }

bool isSuccess(int status) { return status >= 200 && status < 300; }

}  // namespace

// ============================================================================
// App (loop, core 1) から呼ぶ側
// ============================================================================

bool HttpEdgeClient::begin() {
  if (task_ != nullptr) {
    return true;
  }
  mutex_ = xSemaphoreCreateMutex();
  cand_sem_ = xSemaphoreCreateBinary();
  queue_ = xQueueCreate(kQueueDepth, sizeof(Command));
  for (auto& s : slot_) {
    s = static_cast<uint8_t*>(heap_caps_malloc(kSlotBytes, MALLOC_CAP_SPIRAM));
  }
  if (mutex_ == nullptr || cand_sem_ == nullptr || queue_ == nullptr || slot_[0] == nullptr ||
      slot_[1] == nullptr) {
    ESP_LOGE(TAG, "alloc failed (free PSRAM %u)",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    setError("メモリ不足");
    return false;
  }
  http_.setReuse(true);  // keep-alive で 1 本の接続を使い回す (protocol.md)
  http_.setTimeout(static_cast<uint16_t>(config::EDGE_TIMEOUT_MS));
  http_.setConnectTimeout(static_cast<int32_t>(config::EDGE_TIMEOUT_MS));
  http_.setUserAgent("stackchan-photobooth");

  if (xTaskCreatePinnedToCore(taskEntry, "net", kTaskStackBytes, this, kTaskPriority, &task_,
                              kTaskCore) != pdPASS) {
    task_ = nullptr;
    ESP_LOGE(TAG, "net task create failed");
    setError("通信タスクを起動できません");
    return false;
  }
  // 接続先はログに出さない (URL を出さない方針)。ポートだけ出す。
  ESP_LOGI(TAG, "net task started (core %d, stack %u, port %u)", static_cast<int>(kTaskCore),
           static_cast<unsigned>(kTaskStackBytes), static_cast<unsigned>(config::EDGE_PORT));
  return true;
}

bool HttpEdgeClient::isOnline() {
  if (task_ == nullptr || !wifi_.connected() || !ever_ok_.load()) {
    return false;
  }
  if (failures_.load() >= kOfflineAfterFailures) {
    return false;
  }
  // last_ok を先に読んでから millis() を取る (逆だと net タスクの更新で差が負になりうる)。
  const uint32_t last = last_ok_ms_.load();
  return millis() - last < kOnlineWindowMs;
}

void HttpEdgeClient::enqueue(const Command& c) {
  if (queue_ == nullptr || xQueueSend(queue_, &c, 0) != pdTRUE) {
    ESP_LOGW(TAG, "command queue full; dropped kind=%u", static_cast<unsigned>(c.kind));
    setError("コマンドが溢れました");
    return;
  }
  if (task_ != nullptr) {
    xTaskNotifyGive(task_);
  }
}

void HttpEdgeClient::clearMailboxesLocked() {
  has_frame_ = false;
  result_fresh_ = false;
  accepted_pending_ = false;
  frames_stopped_ = false;
  timeout_fresh_ = false;
  photo_fresh_ = false;
  if (cand_buf_ != nullptr) {
    heap_caps_free(cand_buf_);
    cand_buf_ = nullptr;
  }
  cand_len_ = 0;
  cand_wanted_seq_ = 0;
}

void HttpEdgeClient::sessionStart(const app::Session& s) {
  if (task_ == nullptr) return;
  Command c{};
  c.kind = CmdKind::Start;
  c.started_ms = s.started_ms;
  copyStr(c.sid, sizeof(c.sid), s.id);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  ++generation_;  // 以前のセッションの結果は以後捨てる
  c.gen = generation_;
  clearMailboxesLocked();
  xSemaphoreGive(mutex_);
  enqueue(c);
}

void HttpEdgeClient::sessionCancel(const app::Session& s) {
  if (task_ == nullptr) return;
  Command c{};
  c.kind = CmdKind::Cancel;
  copyStr(c.sid, sizeof(c.sid), s.id);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  ++generation_;
  c.gen = generation_;
  clearMailboxesLocked();
  xSemaphoreGive(mutex_);
  enqueue(c);
}

bool HttpEdgeClient::offerFrame(const app::Session& s, const camera_fb_t& fb, int servo_x,
                                int servo_y, edge::Phase phase) {
  if (task_ == nullptr || fb.buf == nullptr || fb.len == 0) {
    return false;
  }
  if (fb.len > kSlotBytes || fb.format != PIXFORMAT_RGB565) {
    ESP_LOGW(TAG, "frame not sent: %u bytes, format %d", static_cast<unsigned>(fb.len),
             static_cast<int>(fb.format));
    return false;
  }
  const uint32_t capture_ms = s.monotonicMs(millis());
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (frames_stopped_) {  // 採用済み: 以後のフレームは edge が dropped を返すだけなので送らない
    xSemaphoreGive(mutex_);
    return false;
  }
  // write 面は App の所有。net タスクは send 面しか読まないので、ここで上書きしてよい。
  memcpy(slot_[write_idx_], fb.buf, fb.len);
  FrameMeta& m = pending_;
  copyStr(m.sid, sizeof(m.sid), s.id);
  m.gen = generation_;
  m.frame_id = s.frame_id;
  m.capture_ms = capture_ms;
  m.servo_x = servo_x;
  m.servo_y = servo_y;
  m.width = static_cast<uint16_t>(fb.width);
  m.height = static_cast<uint16_t>(fb.height);
  m.len = fb.len;
  m.phase = phase;
  has_frame_ = true;
  xSemaphoreGive(mutex_);
  xTaskNotifyGive(task_);
  return true;
}

bool HttpEdgeClient::pollResult(edge::FrameResult& out) {
  if (task_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  bool fresh = false;
  if (accepted_pending_) {
    // accepted は他の結果より先に、App が受け取るまで何度でも返す。
    out = accepted_result_;
    accepted_pending_ = false;
    result_fresh_ = false;  // accepted より後のフレームの結果 (dropped) は要らない
    fresh = true;
  } else if (result_fresh_) {
    out = result_;
    result_fresh_ = false;
    fresh = true;
  }
  xSemaphoreGive(mutex_);
  return fresh;
}

void HttpEdgeClient::sessionTimeout(const app::Session& s) {
  if (task_ == nullptr) return;
  Command c{};
  c.kind = CmdKind::Timeout;
  copyStr(c.sid, sizeof(c.sid), s.id);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  c.gen = generation_;
  timeout_fresh_ = false;
  xSemaphoreGive(mutex_);
  enqueue(c);
}

bool HttpEdgeClient::pollTimeout(bool& ok, bool& has_candidate) {
  if (task_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool fresh = timeout_fresh_;
  if (fresh) {
    ok = timeout_ok_;
    has_candidate = timeout_has_candidate_;
    timeout_fresh_ = false;
  }
  xSemaphoreGive(mutex_);
  return fresh;
}

bool HttpEdgeClient::fetchCandidate(const app::Session& s, uint8_t*& jpeg, size_t& len) {
  jpeg = nullptr;
  len = 0;
  if (task_ == nullptr) return false;
  if (++cand_seq_ == 0) ++cand_seq_;  // 0 は「待っていない」
  const uint32_t seq = cand_seq_;
  Command c{};
  c.kind = CmdKind::Candidate;
  c.seq = seq;
  copyStr(c.sid, sizeof(c.sid), s.id);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (cand_buf_ != nullptr) {  // 前回タイムアウトした依頼の残り
    heap_caps_free(cand_buf_);
    cand_buf_ = nullptr;
  }
  cand_wanted_seq_ = seq;
  c.gen = generation_;
  xSemaphoreGive(mutex_);
  (void)xSemaphoreTake(cand_sem_, 0);  // 前回の依頼の合図が残っていれば捨てる
  enqueue(c);

  const uint32_t t0 = millis();
  const bool signaled =
      xSemaphoreTake(cand_sem_, pdMS_TO_TICKS(config::EDGE_TIMEOUT_MS + kCandidateWaitMarginMs)) ==
      pdTRUE;
  bool ok = false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (cand_done_seq_ == seq && cand_buf_ != nullptr) {
    jpeg = cand_buf_;  // 所有権を呼び出し側へ移す
    len = cand_len_;
    cand_buf_ = nullptr;
    cand_len_ = 0;
    ok = true;
  }
  cand_wanted_seq_ = 0;  // 以後に届いた結果は net タスクが解放する
  xSemaphoreGive(mutex_);
  if (!ok) {
    ESP_LOGW(TAG, "candidate not available (%s, %lu ms)", signaled ? "failed" : "wait timeout",
             static_cast<unsigned long>(millis() - t0));
  }
  return ok;
}

void HttpEdgeClient::reviewDecision(const app::Session& s, bool save) {
  if (task_ == nullptr) return;
  Command c{};
  c.kind = CmdKind::Review;
  c.save = save;
  copyStr(c.sid, sizeof(c.sid), s.id);
  xSemaphoreTake(mutex_, portMAX_DELAY);
  c.gen = generation_;
  photo_fresh_ = false;
  xSemaphoreGive(mutex_);
  enqueue(c);
}

bool HttpEdgeClient::pollPhotoReady(edge::PhotoInfo& out) {
  if (task_ == nullptr) return false;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool fresh = photo_fresh_;
  if (fresh) {
    out = photo_;
    photo_fresh_ = false;
  }
  xSemaphoreGive(mutex_);
  return fresh;
}

const char* HttpEdgeClient::lastError() {
  if (mutex_ == nullptr) {
    return last_error_;  // begin() 前 (net タスクがいないので競合しない)
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  copyStr(error_copy_, sizeof(error_copy_), last_error_);
  xSemaphoreGive(mutex_);
  return error_copy_;
}

void HttpEdgeClient::diagnostics(edge::Diagnostics& out) {
  out = edge::Diagnostics{};
  out.enabled = true;
  const WifiLink::Status st = wifi_.status();
  out.wifi_connected = st == WifiLink::Status::Connected;
  out.wifi_connecting = st == WifiLink::Status::Connecting;
  out.online = isOnline();
  copyStr(out.ssid, sizeof(out.ssid), config::WIFI_SSID);
  wifi_.ip(out.ip, sizeof(out.ip));
  out.rssi = wifi_.rssi();
  copyStr(out.edge_host, sizeof(out.edge_host), config::EDGE_HOST);
  out.edge_port = config::EDGE_PORT;
  const char* reason = wifi_.lastDisconnectReason();
  if (!out.wifi_connected && reason[0] != '\0') {
    snprintf(out.last_error, sizeof(out.last_error), "Wi-Fi: %s", reason);
  } else {
    copyStr(out.last_error, sizeof(out.last_error), lastError());
  }
}

void HttpEdgeClient::reconnect() {
  Command c{};
  c.kind = CmdKind::Reconnect;
  enqueue(c);
}

// ============================================================================
// net タスク (core 0)
// ============================================================================

void HttpEdgeClient::taskEntry(void* arg) { static_cast<HttpEdgeClient*>(arg)->taskLoop(); }

void HttpEdgeClient::taskLoop() {
  stats_since_ms_ = millis();
  for (;;) {
    bool worked = false;

    // (1) コマンド (start / timeout / review / cancel / candidate / reconnect)
    Command c;
    while (xQueueReceive(queue_, &c, 0) == pdTRUE) {
      handleCommand(c);
      worked = true;
    }

    const bool wifi_up = wifi_.connected();
    if (wifi_up != wifi_was_up_) {
      wifi_was_up_ = wifi_up;
      if (!wifi_up) {
        tcp_.stop();  // 古いソケットを使い回さない
        setError("Wi-Fi未接続");
      }
    }

    // (2) 写真の準備待ち (save の後)
    pollPhoto(millis());

    // (3) フレーム。offline の間は送らずに捨てる (hello で復帰を待つ)
    FrameMeta meta;
    if (takeFrame(meta)) {
      if (wifi_up && failures_.load() < kOfflineAfterFailures) {
        sendFrame(meta);
        worked = true;
      } else {
        ++stats_skipped_;
      }
    }

    // (4) 何も送っていない間の接続確認 (offline なら復帰の確認)
    const uint32_t now = millis();
    if (wifi_up && (!requested_once_ || now - last_request_ms_ >= config::HELLO_INTERVAL_MS)) {
      sendHello();
      worked = true;
    }

    logStats(millis());
    if (!worked) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWaitMs));
    }
  }
}

bool HttpEdgeClient::currentGen(uint32_t gen) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool cur = gen == generation_;
  xSemaphoreGive(mutex_);
  return cur;
}

bool HttpEdgeClient::takeFrame(FrameMeta& meta) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (has_frame_ && frames_stopped_ && pending_.gen == generation_) {
    has_frame_ = false;  // 採用済みの世代のフレームは送らない
  }
  const bool has = has_frame_;
  if (has) {
    // 面を入れ替えて所有権を移す (memcpy しない)。以後 App は旧 send 面に書く。
    std::swap(write_idx_, send_idx_);
    meta = pending_;
    has_frame_ = false;
  }
  xSemaphoreGive(mutex_);
  return has;
}

void HttpEdgeClient::dropPendingFrame() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  has_frame_ = false;
  xSemaphoreGive(mutex_);
}

void HttpEdgeClient::handleCommand(const Command& c) {
  char path[96];
  JsonDocument req;
  char json[160];
  switch (c.kind) {
    case CmdKind::Start: {
      req["session_id"] = c.sid;
      req["started_at_ms"] = c.started_ms;
      serializeJson(req, json, sizeof(json));
      poll_active_ = false;
      const Reply r = request("start", "POST", "/v1/sessions", json, nullptr, kMaxJsonBody, true);
      ESP_LOGI(TAG, "session_start %s -> %d", c.sid, r.status);
      break;
    }

    case CmdKind::Timeout: {
      dropPendingFrame();  // 時間切れより前のフレームはもう送らない
      snprintf(path, sizeof(path), "/v1/sessions/%s/timeout", c.sid);
      String body;
      const Reply r = request("timeout", "POST", path, nullptr, &body, kMaxJsonBody, true);
      // 200 で JSON オブジェクトを読めたときだけ Ok。それ以外は「候補なし」ではなく失敗。
      bool ok = false;
      bool has = false;
      if (r.status == 200) {
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok && doc.is<JsonObject>()) {
          ok = true;
          JsonVariantConst cand = doc["candidate"];
          has = !cand.isNull();
          if (has) {
            ESP_LOGI(TAG, "timeout: candidate frame_id=%lu score=%.2f",
                     static_cast<unsigned long>(cand["frame_id"] | 0UL),
                     static_cast<double>(cand["score"] | 0.0f));
          } else {
            ESP_LOGI(TAG, "timeout: no candidate");
          }
        } else {
          setError("timeout: %s", statusText(kErrBadJson));
        }
      }
      if (!ok) {
        ESP_LOGW(TAG, "timeout failed: status %d %s", r.status, r.error_code);
      }
      xSemaphoreTake(mutex_, portMAX_DELAY);
      if (c.gen == generation_) {
        timeout_fresh_ = true;
        timeout_ok_ = ok;
        timeout_has_candidate_ = has;
      }
      xSemaphoreGive(mutex_);
      break;
    }

    case CmdKind::Review: {
      dropPendingFrame();
      snprintf(path, sizeof(path), "/v1/sessions/%s/review", c.sid);
      req["decision"] = c.save ? "save" : "retake";
      serializeJson(req, json, sizeof(json));
      if (!c.save) {
        const Reply r = request("retake", "POST", path, json, nullptr, kMaxJsonBody, true);
        ESP_LOGI(TAG, "review retake -> %d", r.status);
        break;
      }
      // save の再試行はここ 1 か所で数える: 同じ session_id への save の送信は、通信失敗時の
      // 自動再送と App の「再試行」を合わせて合計 UPLOAD_RETRY 回まで (edge 側は冪等、protocol.md)。
      if (strcmp(save_sid_, c.sid) != 0) {
        copyStr(save_sid_, sizeof(save_sid_), c.sid);
        save_sends_ = 0;
      }
      if (save_sends_ >= config::UPLOAD_RETRY) {
        ESP_LOGW(TAG, "review save refused: already sent %u times for this session",
                 static_cast<unsigned>(save_sends_));
        setError("再試行回数を超えました");
        edge::PhotoInfo info;
        info.status = edge::PhotoInfo::Status::Error;
        copyStr(info.reason, sizeof(info.reason), edge::kSaveRetryExhausted);
        publishPhoto(c.gen, info);
        break;
      }
      Reply r;
      while (save_sends_ < config::UPLOAD_RETRY) {
        ++save_sends_;
        r = request("save", "POST", path, json, nullptr, kMaxJsonBody, false);
        ESP_LOGI(TAG, "review save %u/%u -> %d", static_cast<unsigned>(save_sends_),
                 static_cast<unsigned>(config::UPLOAD_RETRY), r.status);
        if (isSuccess(r.status) || (r.status >= 400 && r.status < 500)) {
          break;  // 成功、または送り直しても変わらない 4xx
        }
        if (!currentGen(c.gen) || save_sends_ >= config::UPLOAD_RETRY) {
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(kSaveRetryDelayMs));
      }
      if (isSuccess(r.status)) {
        poll_active_ = true;
        poll_gen_ = c.gen;
        poll_started_ms_ = millis();
        poll_last_ms_ = poll_started_ms_;  // 最初のポーリングは 1 周期後
        copyStr(poll_sid_, sizeof(poll_sid_), c.sid);
      } else {
        edge::PhotoInfo info;
        info.status = edge::PhotoInfo::Status::Error;
        copyStr(info.reason, sizeof(info.reason), r.error_code[0] ? r.error_code : "save_failed");
        publishPhoto(c.gen, info);
      }
      break;
    }

    case CmdKind::Cancel: {
      dropPendingFrame();
      if (poll_active_ && strcmp(poll_sid_, c.sid) == 0) {
        poll_active_ = false;
      }
      snprintf(path, sizeof(path), "/v1/sessions/%s/cancel", c.sid);
      const Reply r = request("cancel", "POST", path, nullptr, nullptr, kMaxJsonBody, true);
      ESP_LOGI(TAG, "session_cancel %s -> %d", c.sid, r.status);
      break;
    }

    case CmdKind::Candidate: {
      uint8_t* buf = nullptr;
      size_t len = 0;
      const bool ok = fetchCandidateNow(c.sid, buf, len);
      xSemaphoreTake(mutex_, portMAX_DELAY);
      if (c.seq == cand_wanted_seq_) {
        if (cand_buf_ != nullptr) {
          heap_caps_free(cand_buf_);
        }
        cand_buf_ = ok ? buf : nullptr;
        cand_len_ = ok ? len : 0;
        cand_done_seq_ = c.seq;
        if (ok) {
          buf = nullptr;  // 所有権は mailbox へ
        }
        xSemaphoreGive(cand_sem_);
      }
      xSemaphoreGive(mutex_);
      if (buf != nullptr) {
        heap_caps_free(buf);  // App はもう待っていない
      }
      break;
    }

    case CmdKind::Reconnect:
      tcp_.stop();
      wifi_.reconnect();
      failures_.store(0);
      requested_once_ = false;  // Wi-Fi が繋がったらすぐ hello
      setError("再接続中");
      break;
  }
}

void HttpEdgeClient::publishPhoto(uint32_t gen, const edge::PhotoInfo& info) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (gen == generation_) {
    photo_ = info;
    photo_fresh_ = true;
  }
  xSemaphoreGive(mutex_);
}

void HttpEdgeClient::pollPhoto(uint32_t now) {
  if (!poll_active_) {
    return;
  }
  if (!currentGen(poll_gen_)) {
    poll_active_ = false;  // 撮り直し・中止
    return;
  }
  if (now - poll_started_ms_ >= config::UPLOAD_WAIT_MS) {
    poll_active_ = false;
    edge::PhotoInfo info;
    info.status = edge::PhotoInfo::Status::Error;
    copyStr(info.reason, sizeof(info.reason), "timeout");
    setError("写真の準備が時間内に終わりません");
    publishPhoto(poll_gen_, info);
    return;
  }
  if (now - poll_last_ms_ < kPhotoPollIntervalMs) {
    return;
  }
  poll_last_ms_ = now;
  char path[96];
  snprintf(path, sizeof(path), "/v1/sessions/%s/photo", poll_sid_);
  String body;
  const Reply r = request("photo", "GET", path, nullptr, &body, kMaxJsonBody, true);
  if (r.status != 200) {
    return;  // 次の周期で再試行 (UPLOAD_WAIT_MS まで)
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    setError("photo: %s", statusText(kErrBadJson));
    return;
  }
  const char* status = doc["status"] | "";
  if (strcmp(status, "pending") == 0) {
    return;
  }
  edge::PhotoInfo info;
  poll_active_ = false;
  if (strcmp(status, "ready") == 0) {
    const char* photo_url = doc["photo_url"] | "";
    const char* share_url = doc["share_url"] | "";
    const char* expires_at = doc["expires_at"] | "";
    if (photo_url[0] == '\0' || strlen(photo_url) >= sizeof(info.photo_url) ||
        strlen(share_url) >= sizeof(info.share_url) ||
        strlen(expires_at) >= sizeof(info.expires_at)) {
      // 切り詰めた URL の QR は出さない (spec §9「QR を捏造しない」)
      info.status = edge::PhotoInfo::Status::Error;
      copyStr(info.reason, sizeof(info.reason), "bad_photo_url");
      setError("photo: 写真URLが不正です");
    } else {
      info.status = edge::PhotoInfo::Status::Ready;
      copyStr(info.photo_url, sizeof(info.photo_url), photo_url);
      copyStr(info.share_url, sizeof(info.share_url), share_url);
      copyStr(info.expires_at, sizeof(info.expires_at), expires_at);
      // URL・トークンはログに出さない (spec §9)
      ESP_LOGI(TAG, "photo ready (expires %s) after %lu ms", info.expires_at,
               static_cast<unsigned long>(now - poll_started_ms_));
    }
  } else {
    info.status = edge::PhotoInfo::Status::Error;
    copyStr(info.reason, sizeof(info.reason), doc["reason"] | "error");
    ESP_LOGW(TAG, "photo error: %s", info.reason);
    setError("photo: %s", info.reason);
  }
  publishPhoto(poll_gen_, info);
}

void HttpEdgeClient::sendHello() {
  JsonDocument req;
  req["device_id"] = config::DEVICE_ID;
  req["protocol_version"] = kProtocolVersion;
  char json[96];
  serializeJson(req, json, sizeof(json));
  const bool was_online = isOnline();
  String body;
  const Reply r = request("hello", "POST", "/v1/hello", json, &body, kMaxJsonBody, false);
  if (r.status != 200 || was_online) {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) {
    return;
  }
  const int countdown = doc["countdown_sec"] | 0;
  ESP_LOGI(TAG, "edge online: ready=%d state=%s max_faces=%d countdown=%d",
           (doc["ready"] | false) ? 1 : 0, doc["edge_state"] | "?", doc["max_faces"] | 0,
           countdown);
  if (countdown != 0 && static_cast<uint32_t>(countdown) != config::COUNTDOWN_SEC) {
    ESP_LOGW(TAG, "countdown differs: edge %d s, device %lu s (device wins)", countdown,
             static_cast<unsigned long>(config::COUNTDOWN_SEC));
  }
}

void HttpEdgeClient::sendFrame(const FrameMeta& meta) {
  char path[96];
  snprintf(path, sizeof(path), "/v1/sessions/%s/frames", meta.sid);
  beginHttp(path);
  http_.addHeader("Content-Type", "application/octet-stream");
  http_.addHeader("X-Frame-Id", String(meta.frame_id));
  http_.addHeader("X-Capture-Ms", String(meta.capture_ms));
  http_.addHeader("X-Servo-X", String(meta.servo_x));
  http_.addHeader("X-Servo-Y", String(meta.servo_y));
  http_.addHeader("X-Width", String(meta.width));
  http_.addHeader("X-Height", String(meta.height));
  http_.addHeader("X-Format", "rgb565");
  http_.addHeader("X-Phase", phaseName(meta.phase));

  const uint32_t t0 = millis();
  // Content-Length は HTTPClient が付ける。失敗しても同じフレームは再送しない (protocol.md)。
  Reply r;
  r.status = http_.POST(slot_[send_idx_], meta.len);
  last_request_ms_ = millis();
  requested_once_ = true;
  String body;
  if (r.status < 0) {
    finishHttp(false);
    noteFailure("frame", r);
    return;
  }
  const int size = http_.getSize();
  if (size > static_cast<int>(kMaxJsonBody)) {
    finishHttp(false);
    r.status = kErrBodyTooLarge;
    noteFailure("frame", r);
    return;
  }
  body = http_.getString();
  finishHttp(true);
  const uint32_t rtt = millis() - t0;

  JsonDocument doc;
  const bool parsed = deserializeJson(doc, body) == DeserializationError::Ok;
  if (r.status != 200) {
    if (parsed) copyStr(r.error_code, sizeof(r.error_code), doc["error"] | "");
    noteFailure("frame", r);  // 撮影中のフレームは 200 以外すべて異常 (unknown_session など)
    return;
  }
  if (!parsed) {
    r.status = kErrBadJson;
    noteFailure("frame", r);
    return;
  }
  const char* sid = doc["session_id"] | "";
  const uint32_t frame_id = doc["frame_id"] | 0UL;
  if (strcmp(sid, meta.sid) != 0 || frame_id != meta.frame_id) {
    r.status = kErrMismatch;
    noteFailure("frame", r);
    return;
  }
  noteSuccess();

  edge::FrameResult fr;
  fr.valid = true;
  fr.frame_id = frame_id;
  fr.dropped = doc["dropped"] | false;
  fr.face_count = clampU8(doc["face_count"] | 0);
  fr.target_face_count = clampU8(doc["target_face_count"] | 0);
  fr.all_in_frame = doc["all_in_frame"] | false;
  fr.all_eyes_open = doc["all_eyes_open"] | false;
  fr.all_smiling = doc["all_smiling"] | false;
  fr.servo_dx = doc["servo_dx"] | 0;
  fr.servo_dy = doc["servo_dy"] | 0;
  fr.hint = parseHint(doc["hint"] | static_cast<const char*>(nullptr));
  fr.accepted = doc["accepted"] | false;
  fr.latency_ms = static_cast<uint16_t>(std::min<uint32_t>(doc["latency_ms"] | 0UL, 65535));

  ++stats_frames_;
  stats_rtt_sum_ += rtt;
  stats_rtt_max_ = std::max(stats_rtt_max_, rtt);
  stats_edge_sum_ += fr.latency_ms;
  ESP_LOGD(TAG, "frame %lu %s: faces %u/%u servo (%d,%d) acc=%d rtt %lu ms edge %u ms",
           static_cast<unsigned long>(fr.frame_id), phaseName(meta.phase), fr.face_count,
           fr.target_face_count, fr.servo_dx, fr.servo_dy, fr.accepted ? 1 : 0,
           static_cast<unsigned long>(rtt), fr.latency_ms);

  // mailbox への書き込み順の約束 (mutex 下):
  //  1. 古い世代 (sessionStart / sessionCancel より前) の結果は捨てる。
  //  2. accepted=true は accepted_result_ に sticky に置き、スロットの保留フレームを捨てて
  //     この世代のフレーム送信を止める。edge は採用後のフレームに dropped=true を返すので、
  //     loop() が accepted を受け取る前に N+1 の応答が来ても accepted は消えない。
  //  3. dropped=true は、まだ受け取られていない dropped でない結果を上書きしない。
  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (meta.gen == generation_) {
    if (fr.accepted) {
      accepted_result_ = fr;
      accepted_pending_ = true;
      frames_stopped_ = true;
      has_frame_ = false;
    } else if (!(fr.dropped && result_fresh_ && !result_.dropped)) {
      result_ = fr;
      result_fresh_ = true;
    }
  }
  xSemaphoreGive(mutex_);
  if (fr.accepted) {
    ESP_LOGI(TAG, "accepted frame_id=%lu; stop sending frames for this session",
             static_cast<unsigned long>(fr.frame_id));
  }
}

bool HttpEdgeClient::fetchCandidateNow(const char* sid, uint8_t*& buf, size_t& len) {
  buf = nullptr;
  len = 0;
  char path[96];
  snprintf(path, sizeof(path), "/v1/sessions/%s/candidate", sid);
  Reply r;
  for (int attempt = 0; attempt < 2; ++attempt) {  // 通信失敗のときだけ 1 回送り直す
    if (!wifi_.connected()) {
      r.status = kErrNoWifi;
      break;
    }
    beginHttp(path);
    r.status = http_.GET();
    if (r.status >= 0) break;
    finishHttp(false);
  }
  last_request_ms_ = millis();
  requested_once_ = true;
  if (r.status < 0) {
    noteFailure("candidate", r);
    return false;
  }
  const int size = http_.getSize();
  if (r.status != 200) {
    if (size >= 0 && size <= static_cast<int>(kMaxJsonBody)) {
      JsonDocument doc;
      if (deserializeJson(doc, http_.getString()) == DeserializationError::Ok) {
        copyStr(r.error_code, sizeof(r.error_code), doc["error"] | "");
      }
      finishHttp(true);
    } else {
      finishHttp(false);
    }
    noteResponse("candidate", r);
    return false;
  }
  if (size <= 0 || static_cast<size_t>(size) > kMaxCandidateBytes) {
    finishHttp(false);
    r.status = kErrBodyTooLarge;
    noteFailure("candidate", r);
    return false;
  }
  buf = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
  if (buf == nullptr) {
    finishHttp(false);
    ESP_LOGE(TAG, "candidate alloc failed (%d bytes)", size);
    setError("candidate: メモリ不足");
    return false;
  }
  NetworkClient* stream = http_.getStreamPtr();
  size_t got = 0;
  while (stream != nullptr && got < static_cast<size_t>(size)) {
    // Stream のタイムアウト (EDGE_TIMEOUT_MS) まで待って読めた分を返す
    const size_t n = stream->readBytes(buf + got, size - got);
    if (n == 0) break;
    got += n;
  }
  if (got != static_cast<size_t>(size)) {
    heap_caps_free(buf);
    buf = nullptr;
    finishHttp(false);
    r.status = HTTPC_ERROR_READ_TIMEOUT;
    noteFailure("candidate", r);
    return false;
  }
  finishHttp(true);
  noteSuccess();
  len = got;
  ESP_LOGI(TAG, "candidate jpeg %u bytes", static_cast<unsigned>(len));
  return true;
}

HttpEdgeClient::Reply HttpEdgeClient::requestOnce(const char* method, const char* path,
                                                  const char* json, String* body_out,
                                                  size_t max_body) {
  Reply r;
  if (!wifi_.connected()) {
    r.status = kErrNoWifi;
    return r;
  }
  beginHttp(path);
  if (strcmp(method, "GET") == 0) {
    r.status = http_.GET();
  } else if (json != nullptr) {
    http_.addHeader("Content-Type", "application/json");
    r.status = http_.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(json)), strlen(json));
  } else {
    http_.addHeader("Content-Length", "0");  // 本文なしの POST
    r.status = http_.POST(static_cast<uint8_t*>(nullptr), 0);
  }
  if (r.status < 0) {
    finishHttp(false);
    return r;
  }
  const int size = http_.getSize();
  if (size > static_cast<int>(max_body)) {
    finishHttp(false);
    r.status = kErrBodyTooLarge;
    return r;
  }
  String body = size != 0 ? http_.getString() : String();
  finishHttp(true);
  if (isSuccess(r.status)) {
    if (body_out != nullptr) *body_out = std::move(body);
  } else {
    JsonDocument doc;
    if (deserializeJson(doc, body) == DeserializationError::Ok) {
      copyStr(r.error_code, sizeof(r.error_code), doc["error"] | "");
    }
  }
  return r;
}

HttpEdgeClient::Reply HttpEdgeClient::request(const char* op, const char* method, const char* path,
                                              const char* json, String* body_out, size_t max_body,
                                              bool retry_transport) {
  Reply r = requestOnce(method, path, json, body_out, max_body);
  if (retry_transport && r.status < 0 && r.status != kErrNoWifi) {
    // keep-alive の接続が edge 側で閉じられていた場合など。新しい接続で 1 回だけ送り直す。
    ESP_LOGW(TAG, "%s: %s (%d); retry once", op, statusText(r.status), r.status);
    r = requestOnce(method, path, json, body_out, max_body);
  }
  last_request_ms_ = millis();
  requested_once_ = true;
  noteResponse(op, r);
  return r;
}

void HttpEdgeClient::noteResponse(const char* op, const Reply& r) {
  // 通信失敗・5xx・401 は接続の異常 (連続失敗に数える)。
  if (r.status < 0 || r.status >= 500 || r.status == 401) {
    noteFailure(op, r);
    return;
  }
  // online の時刻を進めるのは 2xx だけ。404 / 409 などは edge は生きているがセッションが
  // 壊れている応答なので、連続失敗の数は変えず、online の時刻も進めない。
  if (isSuccess(r.status)) {
    noteSuccess();
    return;
  }
  ESP_LOGW(TAG, "%s: http %d %s", op, r.status, r.error_code);
  setError("%s: HTTP %d %s", op, r.status, r.error_code);
}

void HttpEdgeClient::beginHttp(const char* path) {
  http_.begin(tcp_, String(config::EDGE_HOST), config::EDGE_PORT, String(path));
  http_.addHeader("X-Device-Id", config::DEVICE_ID);
  http_.addHeader("X-Device-Key", config::EDGE_SHARED_KEY);
}

void HttpEdgeClient::finishHttp(bool ok) {
  http_.end();  // 応答を読み切っていれば keep-alive の接続は残る
  if (!ok) {
    // 読み残しや途中で切れた応答が次のリクエストに混ざらないよう、接続ごと捨てる。
    tcp_.stop();
  }
}

void HttpEdgeClient::noteSuccess() {
  last_ok_ms_.store(millis());
  ever_ok_.store(true);
  if (failures_.exchange(0) >= kOfflineAfterFailures) {
    ESP_LOGI(TAG, "edge reachable again");
  }
}

void HttpEdgeClient::noteFailure(const char* op, const Reply& r) {
  const uint8_t prev = failures_.load();
  const uint8_t n = prev < 255 ? prev + 1 : prev;
  failures_.store(n);
  ++stats_failures_;
  const char* text = statusText(r.status);
  if (r.status >= 0 && text[0] == '\0') {
    setError("%s: HTTP %d %s", op, r.status, r.error_code);
  } else {
    setError("%s: %s", op, text);
  }
  ESP_LOGW(TAG, "%s failed: status %d %s (%u in a row)", op, r.status, r.error_code,
           static_cast<unsigned>(n));
  if (prev < kOfflineAfterFailures && n >= kOfflineAfterFailures) {
    ESP_LOGW(TAG, "edge offline after %u failures", static_cast<unsigned>(n));
  }
}

void HttpEdgeClient::setError(const char* fmt, ...) {
  char buf[sizeof(last_error_)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (mutex_ == nullptr) {
    copyStr(last_error_, sizeof(last_error_), buf);
    return;
  }
  xSemaphoreTake(mutex_, portMAX_DELAY);
  copyStr(last_error_, sizeof(last_error_), buf);
  xSemaphoreGive(mutex_);
}

void HttpEdgeClient::logStats(uint32_t now) {
  const uint32_t dt = now - stats_since_ms_;
  if (dt < kStatsIntervalMs) {
    return;
  }
  if (stats_frames_ > 0 || stats_failures_ > 0 || stats_skipped_ > 0) {
    const float fps = stats_frames_ * 1000.0f / dt;
    const uint32_t rtt_avg = stats_frames_ ? stats_rtt_sum_ / stats_frames_ : 0;
    const uint32_t edge_avg = stats_frames_ ? stats_edge_sum_ / stats_frames_ : 0;
    ESP_LOGI(TAG,
             "send %lu frames in %lu ms (%.1f fps), rtt avg %lu max %lu ms, edge avg %lu ms, "
             "failures %lu, skipped %lu, stack free %u",
             static_cast<unsigned long>(stats_frames_), static_cast<unsigned long>(dt), fps,
             static_cast<unsigned long>(rtt_avg), static_cast<unsigned long>(stats_rtt_max_),
             static_cast<unsigned long>(edge_avg), static_cast<unsigned long>(stats_failures_),
             static_cast<unsigned long>(stats_skipped_),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  }
  stats_since_ms_ = now;
  stats_frames_ = 0;
  stats_failures_ = 0;
  stats_skipped_ = 0;
  stats_rtt_sum_ = 0;
  stats_rtt_max_ = 0;
  stats_edge_sum_ = 0;
}

}  // namespace net
