/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
#include "http_edge_client.h"

#include "null_edge.h"

namespace photobooth::net {

std::unique_ptr<EdgeClient> createEdgeClient()
{
#if PHOTOBOOTH_EDGE_ENABLED
    return std::make_unique<HttpEdgeClient>();
#else
    return std::make_unique<NullEdge>();
#endif
}

}  // namespace photobooth::net

#if PHOTOBOOTH_EDGE_ENABLED

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

#include "../view/strings.h"
#include "edge_config.h"
#include "network.h"

namespace photobooth::net {

namespace {

namespace local = config::local;

constexpr const char* kTag = "PB-Edge";

constexpr int kProtocolVersion = 1;
// esp_http_client + ArduinoJson + fmt の分。高水位は logStats() で出す。
constexpr uint32_t kTaskStack   = 8192;
constexpr UBaseType_t kTaskPrio = 3;  // メインループ (1) より上、Wi-Fi / lwIP より下
constexpr UBaseType_t kQueueDepth = 8;
// この回数続けて失敗したら offline にしてフレームを送らず、hello で復帰を待つ。
constexpr uint8_t kOfflineAfterFailures = 3;
// hello は HELLO_INTERVAL_MS ごとなので、1 回の hello の往復で offline に見えないよう窓は 2 倍にする。
constexpr uint32_t kOnlineWindowMs      = config::HELLO_INTERVAL_MS * 2;
constexpr uint32_t kPhotoPollIntervalMs = 500;
constexpr uint32_t kSaveRetryDelayMs    = 300;
constexpr size_t kMaxJsonBody           = 2048;        // frame_result / photo は 1 KB 未満
constexpr size_t kMaxCandidateBytes     = 256 * 1024;  // QVGA 品質 80 の JPEG は数十 KB
constexpr size_t kBinaryInitialBytes    = 16 * 1024;   // 長さ不明 (chunked) の候補 JPEG を読み始める大きさ
constexpr uint32_t kCandidateWaitMarginMs = 500;
constexpr uint32_t kStatsIntervalMs     = 5000;
constexpr uint32_t kIdleWaitMs          = 20;
constexpr int kHttpRxBuf                = 1024;
constexpr int kHttpTxBuf                = 1024;  // 1 行目 (メソッド + パス) とヘッダ 1 本ずつが入る大きさ
constexpr size_t kWriteChunk            = 8192;  // フレーム本文を書く単位 (合間に期限と終了要求を見る)
constexpr size_t kReadChunk             = 4096;  // 応答本文を読む単位 (同上)

// QVGA RGB565 1 枚分。これより大きいフレームは offerFrame() で断る。
constexpr size_t kSlotBytes = 320 * 240 * 2;

// 通信の失敗を表す負の status (HTTP の status は正)。
constexpr int kErrNoWifi       = -100;
constexpr int kErrBodyTooLarge = -101;
constexpr int kErrBadJson      = -102;
constexpr int kErrMismatch     = -103;
constexpr int kErrConnect      = -110;  // TCP 接続できない
constexpr int kErrSend         = -111;  // ヘッダ・本文を送れない
constexpr int kErrLost         = -112;  // 応答の途中で切れた
constexpr int kErrTimeout      = -113;  // 応答が時間内に来ない
constexpr int kErrInternal     = -114;  // クライアントを作れない (メモリ不足など)
constexpr int kErrAborted      = -115;  // アプリを閉じる途中

static_assert(config::UPLOAD_RETRY >= 1, "UPLOAD_RETRY must be >= 1");

// frame リクエストだけに付けるヘッダ (protocol.md)。他のリクエストの前に消す。
constexpr const char* kFrameHeaders[] = {
    "X-Frame-Id", "X-Capture-Ms", "X-Servo-X", "X-Servo-Y", "X-Width", "X-Height", "X-Format", "X-Phase",
};

uint32_t nowMs()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);  // GetHAL().millis() と同じ時計
}

void copyStr(char* dst, size_t n, const char* src)
{
    snprintf(dst, n, "%s", src != nullptr ? src : "");
}

Hint parseHint(const char* h)
{
    if (h == nullptr) return Hint::None;
    if (strcmp(h, "closer") == 0) return Hint::Closer;
    if (strcmp(h, "too_many") == 0) return Hint::TooMany;
    return Hint::None;
}

const char* phaseName(Phase p)
{
    return p == Phase::Capture ? "capture" : "compose";
}

// 画面 (診断・ERROR) に出す短い説明。URL や鍵を含めない。
const char* statusText(int status)
{
    switch (status) {
        case kErrNoWifi:
            return str::kNetNoWifi;
        case kErrBodyTooLarge:
            return str::kNetBodyTooLarge;
        case kErrBadJson:
            return str::kNetBadJson;
        case kErrMismatch:
            return str::kNetMismatch;
        case kErrConnect:
            return str::kNetConnectFailed;
        case kErrSend:
            return str::kNetSendFailed;
        case kErrLost:
            return str::kNetLost;
        case kErrTimeout:
            return str::kNetNoResponse;
        case kErrInternal:
            return str::kErrNoMemory;
        case 401:
            return str::kNetUnauthorized;
        default:
            return status < 0 ? str::kNetError : "";
    }
}

uint8_t clampU8(int v)
{
    return static_cast<uint8_t>(std::min(std::max(v, 0), 255));
}

bool isSuccess(int status)
{
    return status >= 200 && status < 300;
}

bool wifiUp()
{
    return Network::status() == Network::Status::Connected;
}

}  // namespace

// net タスクと HttpEdgeClient が共有する状態。両方が shared_ptr で持ち、最後に手放した側が解放する。
// end() がタスクを待ちきれずに切り離しても、タスクが終わるときにここで後始末される。
struct EdgeWorker {
    enum class CmdKind : uint8_t { Start, Timeout, Review, Cancel, Candidate, Reconnect };
    struct Command {
        CmdKind kind;
        bool save;            // Review
        uint32_t gen;         // 依頼時の世代
        uint32_t seq;         // Candidate の依頼番号
        uint32_t started_ms;  // Start
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
        Phase phase;
    };
    // 1 回の HTTP のやりとりの結果。status < 0 は通信失敗。
    struct Reply {
        int status          = 0;
        char error_code[40] = {};  // エラー応答の {"error": ...}
    };
    struct Header {
        const char* name;
        const char* value;
    };
    struct Request {
        esp_http_client_method_t method = HTTP_METHOD_POST;
        const char* path                = "/";
        const char* content_type        = nullptr;
        const uint8_t* body             = nullptr;
        size_t body_len                 = 0;
        const Header* frame_headers     = nullptr;  // frame のときだけ (kFrameHeaders と同じ並び)
        JpegBytes* binary_out = nullptr;  // 200 の本文をここへ (候補 JPEG)
    };

    ~EdgeWorker();
    bool allocate();
    void run();

    // ---- どちらのスレッドからも ----
    void setError(const char* text);
    void setErrorOp(const char* op, const char* text);
    bool enqueue(const Command& c);
    void clearMailboxesLocked();  // mutex を持って呼ぶ
    bool online() const;

    // ---- net タスクだけが呼ぶ ----
    void handleCommand(const Command& c);
    bool takeFrame(FrameMeta& meta);
    void dropPendingFrame();
    void sendFrame(const FrameMeta& meta);
    void sendHello();
    void pollPhoto(uint32_t now);
    void publishPhoto(uint32_t gen, const PhotoInfo& info);
    void fetchCandidate(const char* sid, bool& ok, JpegBytes& jpeg);
    // JSON / 空本文のリクエスト。応答本文は json (NUL 終端、json_len バイト) に入る。
    // retry_transport なら通信失敗 (status < 0) のとき接続を作り直して 1 回だけ送り直す
    // (keep-alive の接続が edge 側で閉じられていた場合の対策。冪等なリクエストだけ)。
    Reply request(const char* op, esp_http_client_method_t method, const char* path, const char* body,
                  bool retry_transport);
    Reply exchange(const Request& rq);
    bool ensureHttp();
    bool armDeadline();
    Reply exchangeGuarded(const Request& rq);
    void armWatchdog();
    bool watchdogSetFd(int fd);
    void disarmWatchdog();
    bool watchdogFired();
    static void onDeadline(void* arg);
    enum class ReadStep : uint8_t { Data, End, Again, Error };
    ReadStep readSome(char* dst, size_t want, size_t& n, int& err);
    int readBody(char* dst, size_t cap, size_t& got);
    int readBinary(JpegBytes& out, int64_t content_len);
    void closeConnection();
    void dropHttp();
    void dropIfPeerClosed();
    void noteResponse(const char* op, const Reply& r);
    void noteSuccess();
    void noteFailure(const char* op, const Reply& r);
    void logStats(uint32_t now);
    bool currentGen(uint32_t gen);

    QueueHandle_t queue    = nullptr;
    SemaphoreHandle_t wake = nullptr;  // タスクを起こす (タスクハンドルを使わないので、終了後に叩いても安全)
    std::mutex mutex;
    std::atomic<bool> quit{false};
    std::atomic<bool> running{false};

    // ---- mutex で守る ----
    uint32_t generation = 0;
    uint8_t* slot[2]    = {nullptr, nullptr};
    int write_idx       = 0;  // Flow が書く面。もう一方 (send_idx) は net タスクの所有
    bool has_frame      = false;
    FrameMeta pending{};
    FrameResult result{};
    bool result_fresh = false;
    // accepted=true の結果は result とは別に持ち、Flow が受け取るまで消さない (sticky)。
    // 後から届いた dropped=true の応答で上書きされないようにするため。
    FrameResult accepted_result{};
    bool accepted_pending = false;
    // accepted を受けた世代ではフレームを送らない (sessionStart / sessionCancel で解除)。
    bool frames_stopped        = false;
    bool timeout_fresh         = false;
    bool timeout_ok            = false;
    bool timeout_has_candidate = false;
    PhotoInfo photo{};
    bool photo_fresh         = false;
    uint32_t cand_wanted_seq = 0;  // Flow が待っている依頼番号 (0 = 待っていない)
    uint32_t cand_done_seq   = 0;
    bool cand_ok             = false;
    JpegBytes cand_jpeg;
    char last_error[96] = {};

    // ---- net タスクと Flow の両方から読む ----
    std::atomic<uint32_t> last_ok_ms{0};
    std::atomic<bool> ever_ok{false};
    std::atomic<uint8_t> failures{0};  // 連続失敗回数

    // ---- 期限の番人 (wd_mutex で守る。net タスクと esp_timer タスクが触る) ----
    // リクエスト全体の期限 (EDGE_TIMEOUT_MS) に達したら、esp_timer のコールバックがソケットを
    // shutdown() して、esp_http_client の中でブロックしている select / recv / send を失敗で戻らせる
    // (open() や fetch_headers() は内部でループし、1 回の送受信ごとにタイムアウトを数え直すので、
    //  少しずつバイトを送ってくる相手だと armDeadline() だけでは期限を越えて待ち続ける)。
    // ロックの順序: wd_mutex は末端のロック。持ったまま mutex (mailbox) を取らない。コールバックは
    // wd_mutex だけを取る。fd を閉じる (closeConnection / dropHttp) 前に必ず disarmWatchdog() で
    // armed=false にするので、コールバックが再利用された fd を shutdown することは無い。
    std::mutex wd_mutex;
    esp_timer_handle_t wd_timer = nullptr;
    int wd_fd                   = -1;     // 今のリクエストが使っているソケット (接続前は -1)
    bool wd_armed               = false;  // リクエストの最中
    bool wd_fired               = false;  // 期限に達した (exchange() が受け取るまで残る)

    // ---- net タスクだけが触る ----
    esp_http_client_handle_t http = nullptr;
    bool http_open                = false;  // TCP 接続を持っている (と思っている)
    bool sending_cancel           = false;  // 終了要求の後でも送ってよいリクエスト (session_cancel) の最中
    uint32_t request_started_ms   = 0;      // 今のリクエストの期限の起点 (exchange の中だけで使う)
    char json[kMaxJsonBody + 1]   = {};     // 直近の応答本文
    size_t json_len               = 0;
    int send_idx                  = 1;
    uint32_t last_request_ms      = 0;
    bool requested_once           = false;
    bool wifi_was_up              = false;
    bool poll_active              = false;  // save の後の GET …/photo ポーリング
    uint32_t poll_gen             = 0;
    uint32_t poll_started_ms      = 0;
    uint32_t poll_last_ms         = 0;
    char poll_sid[37]             = {};
    // save を送った回数 (session_id ごと、合計 UPLOAD_RETRY 回まで)
    char save_sid[37]  = {};
    uint8_t save_sends = 0;
    // 送信 fps と往復時間 (logStats で出してリセット)
    uint32_t stats_since_ms = 0;
    uint32_t stats_frames   = 0;
    uint32_t stats_failures = 0;
    uint32_t stats_skipped  = 0;  // offline で送らずに捨てたフレーム
    uint32_t stats_rtt_sum  = 0;
    uint32_t stats_rtt_max  = 0;
    uint32_t stats_edge_sum = 0;
};

namespace {

// 切り離したタスクの状態。HttpEdgeClient はアプリを開くたびに作り直されるので、ファイルスコープで覚える。
std::weak_ptr<EdgeWorker> g_detached;

void taskEntry(void* arg)
{
    // 引数はタスク用の shared_ptr のコピー (new で渡したもの)
    auto* holder = static_cast<std::shared_ptr<EdgeWorker>*>(arg);
    (*holder)->run();
    (*holder)->running = false;
    delete holder;  // 切り離されていれば、ここで EdgeWorker が解放される
    vTaskDelete(nullptr);
}

}  // namespace

// ============================================================================
// HttpEdgeClient: Flow (onRunning のスレッド) から呼ぶ側
// ============================================================================

HttpEdgeClient::~HttpEdgeClient()
{
    end();
}

bool HttpEdgeClient::busy()
{
    auto w = g_detached.lock();
    return w != nullptr && w->running.load();
}

bool HttpEdgeClient::begin()
{
    if (worker_) {
        return true;
    }
    if (busy()) {
        // 前の net タスクがまだ通信の途中 (タイムアウト待ち)。二重に回さない。
        mclog::tagError(kTag, "previous net task is still running; edge disabled until reconnect");
        copyStr(begin_error_, sizeof(begin_error_), str::kNetTaskBusy);
        return false;
    }
    auto w = std::make_shared<EdgeWorker>();
    if (!w->allocate()) {
        mclog::tagError(kTag, "alloc failed (free PSRAM {})", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        copyStr(begin_error_, sizeof(begin_error_), str::kErrNoMemory);
        return false;
    }
    w->running   = true;
    auto* holder = new std::shared_ptr<EdgeWorker>(w);
    if (xTaskCreate(taskEntry, "pb_net", kTaskStack, holder, kTaskPrio, nullptr) != pdPASS) {
        mclog::tagError(kTag, "net task create failed");
        delete holder;
        copyStr(begin_error_, sizeof(begin_error_), str::kNetTaskFailed);
        return false;
    }
    worker_         = std::move(w);
    begin_error_[0] = '\0';
    cand_waiting_   = false;
    // 接続先はログに出さない (URL を出さない方針)。ポートだけ出す。
    mclog::tagInfo(kTag, "net task started (stack {}, port {})", kTaskStack, static_cast<unsigned>(local::kEdgePort));
    return true;
}

void HttpEdgeClient::end()
{
    if (!worker_) {
        return;
    }
    auto& w = *worker_;
    w.quit  = true;
    xSemaphoreGive(w.wake);
    // 通信の途中 (最大 EDGE_TIMEOUT_MS のタイムアウト待ち) でなければすぐ終わる。
    for (int i = 0; i < 100 && w.running.load(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (w.running.load()) {
        mclog::tagWarn(kTag, "net task did not stop in 1 s; detached (it cleans up when the request ends)");
        g_detached = worker_;
    } else {
        mclog::tagInfo(kTag, "net task stopped");
    }
    worker_.reset();  // Wi-Fi は切らない (純正の他のアプリが使う)
    cand_waiting_ = false;
}

bool HttpEdgeClient::isOnline()
{
    return worker_ != nullptr && wifiUp() && worker_->online();
}

LinkState HttpEdgeClient::linkState()
{
    return isOnline() ? LinkState::Online : LinkState::Offline;
}

void HttpEdgeClient::sessionStart(const Session& s)
{
    cand_waiting_ = false;
    if (!worker_) return;
    auto& w = *worker_;
    EdgeWorker::Command c{};
    c.kind       = EdgeWorker::CmdKind::Start;
    c.started_ms = s.started_ms;
    copyStr(c.sid, sizeof(c.sid), s.id);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        ++w.generation;  // 以前のセッションの結果は以後捨てる
        c.gen = w.generation;
        w.clearMailboxesLocked();
    }
    w.enqueue(c);
}

void HttpEdgeClient::sessionCancel(const Session& s)
{
    cand_waiting_ = false;
    if (!worker_) return;
    auto& w = *worker_;
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Cancel;
    copyStr(c.sid, sizeof(c.sid), s.id);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        ++w.generation;
        c.gen = w.generation;
        w.clearMailboxesLocked();
    }
    w.enqueue(c);
}

bool HttpEdgeClient::offerFrame(const Session& s, const hw::FrameView& frame, int servo_x, int servo_y, Phase phase)
{
    if (!worker_ || frame.pixels == nullptr || frame.width == 0 || frame.height == 0) {
        return false;
    }
    auto& w          = *worker_;
    const size_t len = static_cast<size_t>(frame.width) * frame.height * 2;
    if (len > kSlotBytes) {
        mclog::tagWarn(kTag, "frame not sent: {}x{} is larger than the slot", frame.width, frame.height);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        if (w.frames_stopped) {  // 採用済み: 以後のフレームは edge が dropped を返すだけなので送らない
            return false;
        }
        // write 面は Flow の所有。net タスクは send 面しか読まないので、ここで上書きしてよい。
        memcpy(w.slot[w.write_idx], frame.pixels, len);
        EdgeWorker::FrameMeta& m = w.pending;
        copyStr(m.sid, sizeof(m.sid), s.id);
        m.gen        = w.generation;
        m.frame_id   = s.frame_id;
        m.capture_ms = s.monotonicMs(frame.captured_ms);  // カメラから取った時刻 (セッション開始からの ms)
        m.servo_x    = servo_x;
        m.servo_y    = servo_y;
        m.width      = frame.width;
        m.height     = frame.height;
        m.len        = len;
        m.phase      = phase;
        w.has_frame  = true;
    }
    xSemaphoreGive(w.wake);
    return true;
}

bool HttpEdgeClient::pollResult(FrameResult& out)
{
    if (!worker_) return false;
    auto& w = *worker_;
    std::lock_guard<std::mutex> lock(w.mutex);
    if (w.accepted_pending) {
        // accepted は他の結果より先に返す。
        out                = w.accepted_result;
        w.accepted_pending = false;
        w.result_fresh     = false;  // accepted より後のフレームの結果 (dropped) は要らない
        return true;
    }
    if (w.result_fresh) {
        out            = w.result;
        w.result_fresh = false;
        return true;
    }
    return false;
}

void HttpEdgeClient::sessionTimeout(const Session& s)
{
    if (!worker_) return;
    auto& w = *worker_;
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Timeout;
    copyStr(c.sid, sizeof(c.sid), s.id);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        c.gen           = w.generation;
        w.timeout_fresh = false;
    }
    if (!w.enqueue(c)) {
        // 依頼できなかった: Flow が待ち続けないよう、失敗として返す。
        std::lock_guard<std::mutex> lock(w.mutex);
        w.timeout_fresh = true;
        w.timeout_ok    = false;
    }
}

bool HttpEdgeClient::pollTimeout(bool& ok, bool& has_candidate)
{
    if (!worker_) return false;
    auto& w = *worker_;
    std::lock_guard<std::mutex> lock(w.mutex);
    if (!w.timeout_fresh) {
        return false;
    }
    ok              = w.timeout_ok;
    has_candidate   = w.timeout_has_candidate;
    w.timeout_fresh = false;
    return true;
}

void HttpEdgeClient::requestCandidate(const Session& s)
{
    cand_waiting_      = true;
    cand_requested_ms_ = nowMs();
    if (!worker_) return;  // pollCandidate() がすぐ失敗を返す
    auto& w = *worker_;
    if (++cand_seq_ == 0) ++cand_seq_;  // 0 は「待っていない」
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Candidate;
    c.seq  = cand_seq_;
    copyStr(c.sid, sizeof(c.sid), s.id);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.cand_jpeg.clear();  // 前回タイムアウトした依頼の残り
        w.cand_wanted_seq = cand_seq_;
        c.gen             = w.generation;
    }
    if (!w.enqueue(c)) {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.cand_done_seq = cand_seq_;
        w.cand_ok       = false;
    }
}

bool HttpEdgeClient::pollCandidate(bool& ok, JpegBytes& jpeg)
{
    if (!cand_waiting_) {
        return false;
    }
    if (!worker_) {
        cand_waiting_ = false;
        ok            = false;
        return true;
    }
    auto& w                = *worker_;
    const uint32_t waited  = nowMs() - cand_requested_ms_;
    bool done              = false;
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        if (w.cand_done_seq == cand_seq_) {
            ok = w.cand_ok && !w.cand_jpeg.empty();
            if (ok) {
                jpeg = std::move(w.cand_jpeg);  // 所有権を呼び出し側へ移す
            }
            w.cand_jpeg.clear();
            done = true;
        } else if (waited >= config::EDGE_TIMEOUT_MS + kCandidateWaitMarginMs) {
            ok   = false;
            done = true;
        }
        if (done) {
            w.cand_wanted_seq = 0;  // 以後に届いた結果は net タスクが捨てる
        }
    }
    if (done) {
        cand_waiting_ = false;
        if (!ok) {
            mclog::tagWarn(kTag, "candidate not available after {} ms", waited);
        }
    }
    return done;
}

void HttpEdgeClient::reviewDecision(const Session& s, bool save)
{
    if (!worker_) return;
    auto& w = *worker_;
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Review;
    c.save = save;
    copyStr(c.sid, sizeof(c.sid), s.id);
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        c.gen         = w.generation;
        w.photo_fresh = false;
    }
    if (!w.enqueue(c) && save) {
        PhotoInfo info;
        info.status = PhotoInfo::Status::Error;
        copyStr(info.reason, sizeof(info.reason), "queue_full");
        w.publishPhoto(c.gen, info);
    }
}

bool HttpEdgeClient::pollPhotoReady(PhotoInfo& out)
{
    if (!worker_) return false;
    auto& w = *worker_;
    std::lock_guard<std::mutex> lock(w.mutex);
    if (!w.photo_fresh) {
        return false;
    }
    out           = w.photo;
    w.photo_fresh = false;
    return true;
}

const char* HttpEdgeClient::lastError()
{
    if (!worker_) {
        return begin_error_;
    }
    std::lock_guard<std::mutex> lock(worker_->mutex);
    copyStr(error_copy_, sizeof(error_copy_), worker_->last_error);
    return error_copy_;
}

void HttpEdgeClient::diagnostics(Diagnostics& out)
{
    out         = Diagnostics{};
    out.enabled = true;
    switch (Network::status()) {
        case Network::Status::NotConfigured:
            out.wifi = Diagnostics::Wifi::NotConfigured;
            break;
        case Network::Status::Disconnected:
            out.wifi = Diagnostics::Wifi::Disconnected;
            break;
        case Network::Status::ConfigMode:
            out.wifi = Diagnostics::Wifi::ConfigMode;
            break;
        case Network::Status::Connected:
            out.wifi = Diagnostics::Wifi::Connected;
            break;
    }
    out.online = isOnline();
    Network::info(out.ssid, sizeof(out.ssid), out.ip, sizeof(out.ip), out.rssi);
    copyStr(out.edge_host, sizeof(out.edge_host), local::kEdgeHost);
    out.edge_port = local::kEdgePort;
    if (out.wifi == Diagnostics::Wifi::NotConfigured) {
        copyStr(out.last_error, sizeof(out.last_error), str::kNetWifiNotConfigured);
    } else if (out.wifi != Diagnostics::Wifi::Connected && worker_) {
        copyStr(out.last_error, sizeof(out.last_error), str::kNetNoWifi);
    } else {
        copyStr(out.last_error, sizeof(out.last_error), lastError());
    }
}

void HttpEdgeClient::reconnect()
{
    // Wi-Fi には触らない: 再接続そのものは純正 (WifiManager の自動再接続) に任せる。Wi-Fi を起動して
    // いない (アプリを開いたときに未設定だった) 場合は、SETUP で設定してからアプリを開き直す。
    if (!worker_) {
        begin();  // 前のタスクが切り離されたまま (busy) で始められなかったときのやり直し
        return;
    }
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Reconnect;
    worker_->enqueue(c);
}

// ============================================================================
// EdgeWorker: 共有状態
// ============================================================================

bool EdgeWorker::allocate()
{
    wake  = xSemaphoreCreateBinary();
    queue = xQueueCreate(kQueueDepth, sizeof(Command));
    for (auto& s : slot) {
        s = static_cast<uint8_t*>(heap_caps_malloc(kSlotBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    esp_timer_create_args_t args = {};
    args.callback                = &EdgeWorker::onDeadline;
    args.arg                     = this;
    args.dispatch_method         = ESP_TIMER_TASK;
    args.name                    = "pb_edge_dl";
    if (esp_timer_create(&args, &wd_timer) != ESP_OK) {
        wd_timer = nullptr;
    }
    return wake != nullptr && queue != nullptr && slot[0] != nullptr && slot[1] != nullptr && wd_timer != nullptr;
}

EdgeWorker::~EdgeWorker()
{
    // 最後の shared_ptr を手放した側 (通常は end()、切り離し時は net タスク) で走る。
    // この時点で net タスクは run() を抜けているので、http はもう使われていない。
    dropHttp();  // 先に disarmWatchdog() する
    if (wd_timer != nullptr) {
        esp_timer_stop(wd_timer);  // 動いていなければ ESP_ERR_INVALID_STATE (無視)
        esp_timer_delete(wd_timer);
        wd_timer = nullptr;
    }
    for (auto& s : slot) {
        if (s != nullptr) {
            heap_caps_free(s);
            s = nullptr;
        }
    }
    if (queue != nullptr) {
        vQueueDelete(queue);
    }
    if (wake != nullptr) {
        vSemaphoreDelete(wake);
    }
}

bool EdgeWorker::online() const
{
    if (!ever_ok.load() || failures.load() >= kOfflineAfterFailures) {
        return false;
    }
    // last_ok を先に読んでから now を取る (逆だと net タスクの更新で差が負になりうる)。
    const uint32_t last = last_ok_ms.load();
    return nowMs() - last < kOnlineWindowMs;
}

bool EdgeWorker::enqueue(const Command& c)
{
    if (xQueueSend(queue, &c, 0) != pdTRUE) {
        mclog::tagWarn(kTag, "command queue full; dropped kind={}", static_cast<unsigned>(c.kind));
        setError(str::kNetQueueFull);
        return false;
    }
    xSemaphoreGive(wake);
    return true;
}

void EdgeWorker::clearMailboxesLocked()
{
    has_frame        = false;
    result_fresh     = false;
    accepted_pending = false;
    frames_stopped   = false;
    timeout_fresh    = false;
    photo_fresh      = false;
    cand_jpeg.clear();
    cand_jpeg.shrink_to_fit();
    cand_wanted_seq = 0;
}

void EdgeWorker::setError(const char* text)
{
    std::lock_guard<std::mutex> lock(mutex);
    copyStr(last_error, sizeof(last_error), text);
}

void EdgeWorker::setErrorOp(const char* op, const char* text)
{
    std::lock_guard<std::mutex> lock(mutex);
    snprintf(last_error, sizeof(last_error), "%s: %s", op, text);
}

void EdgeWorker::publishPhoto(uint32_t gen, const PhotoInfo& info)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (gen == generation) {
        photo       = info;
        photo_fresh = true;
    }
}

// ============================================================================
// net タスク
// ============================================================================

void EdgeWorker::run()
{
    stats_since_ms = nowMs();
    for (;;) {
        bool worked = false;

        // (1) コマンド (start / timeout / review / cancel / candidate / reconnect)
        Command c;
        while (xQueueReceive(queue, &c, 0) == pdTRUE) {
            if (quit.load() && c.kind != CmdKind::Cancel) {
                continue;  // アプリを閉じる途中: 未公開の候補を捨てさせる cancel だけ届ける
            }
            handleCommand(c);
            worked = true;
        }
        if (quit.load()) {
            break;
        }

        const bool wifi_up = wifiUp();
        if (wifi_up != wifi_was_up) {
            wifi_was_up = wifi_up;
            mclog::tagInfo(kTag, "wifi {}", wifi_up ? "up" : "down");
            if (!wifi_up) {
                dropHttp();  // 古いソケットを使い回さない
                setError(str::kNetNoWifi);
            }
        }

        // (2) 写真の準備待ち (save の後)
        pollPhoto(nowMs());

        // (3) フレーム。offline の間は送らずに捨てる (hello で復帰を待つ)
        FrameMeta meta;
        if (takeFrame(meta)) {
            if (wifi_up && failures.load() < kOfflineAfterFailures) {
                sendFrame(meta);
                worked = true;
            } else {
                ++stats_skipped;
            }
        }

        // (4) 何も送っていない間の接続確認 (offline なら復帰の確認)。HELLO_INTERVAL_MS ごと
        const uint32_t now = nowMs();
        if (wifi_up && !quit.load() && (!requested_once || now - last_request_ms >= config::HELLO_INTERVAL_MS)) {
            sendHello();
            worked = true;
        }

        logStats(nowMs());
        if (!worked) {
            xSemaphoreTake(wake, pdMS_TO_TICKS(kIdleWaitMs));
        }
    }
    dropHttp();
    mclog::tagInfo(kTag, "net task exit (stack free {})", uxTaskGetStackHighWaterMark(nullptr));
}

bool EdgeWorker::currentGen(uint32_t gen)
{
    std::lock_guard<std::mutex> lock(mutex);
    return gen == generation;
}

bool EdgeWorker::takeFrame(FrameMeta& meta)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (has_frame && (pending.gen != generation || frames_stopped)) {
        has_frame = false;  // 古い世代・採用済みの世代のフレームは送らない
    }
    if (!has_frame) {
        return false;
    }
    // 面を入れ替えて所有権を移す (memcpy しない)。以後 Flow は旧 send 面に書く。
    std::swap(write_idx, send_idx);
    meta      = pending;
    has_frame = false;
    return true;
}

void EdgeWorker::dropPendingFrame()
{
    std::lock_guard<std::mutex> lock(mutex);
    has_frame = false;
}

void EdgeWorker::handleCommand(const Command& c)
{
    char path[96];
    char body[160];
    JsonDocument req;
    switch (c.kind) {
        case CmdKind::Start: {
            req["session_id"]    = c.sid;
            req["started_at_ms"] = c.started_ms;
            serializeJson(req, body, sizeof(body));
            poll_active   = false;
            const Reply r = request("start", HTTP_METHOD_POST, "/v1/sessions", body, true);
            mclog::tagInfo(kTag, "session_start -> {}", r.status);
            break;
        }

        case CmdKind::Timeout: {
            dropPendingFrame();  // 時間切れより前のフレームはもう送らない
            snprintf(path, sizeof(path), "/v1/sessions/%s/timeout", c.sid);
            const Reply r = request("timeout", HTTP_METHOD_POST, path, nullptr, true);
            // 200 で JSON オブジェクトを読めたときだけ ok。それ以外は「候補なし」ではなく失敗。
            bool ok  = false;
            bool has = false;
            if (r.status == 200) {
                JsonDocument doc;
                if (deserializeJson(doc, static_cast<const char*>(json), json_len) == DeserializationError::Ok &&
                    doc.is<JsonObject>()) {
                    ok                     = true;
                    JsonVariantConst cand  = doc["candidate"];
                    has                    = !cand.isNull();
                    if (has) {
                        mclog::tagInfo(kTag, "timeout: candidate frame_id={} score={:.2f}",
                                       static_cast<unsigned long>(cand["frame_id"] | 0UL),
                                       static_cast<double>(cand["score"] | 0.0f));
                    } else {
                        mclog::tagInfo(kTag, "timeout: no candidate");
                    }
                } else {
                    setErrorOp("timeout", statusText(kErrBadJson));
                }
            }
            if (!ok) {
                mclog::tagWarn(kTag, "timeout failed: status {} {}", r.status, static_cast<const char*>(r.error_code));
            }
            std::lock_guard<std::mutex> lock(mutex);
            if (c.gen == generation) {
                timeout_fresh         = true;
                timeout_ok            = ok;
                timeout_has_candidate = has;
            }
            break;
        }

        case CmdKind::Review: {
            dropPendingFrame();
            snprintf(path, sizeof(path), "/v1/sessions/%s/review", c.sid);
            req["decision"] = c.save ? "save" : "retake";
            serializeJson(req, body, sizeof(body));
            if (!c.save) {
                const Reply r = request("retake", HTTP_METHOD_POST, path, body, true);
                mclog::tagInfo(kTag, "review retake -> {}", r.status);
                break;
            }
            // save の再試行はここ 1 か所で数える: 同じ session_id への save の送信は、通信失敗時の
            // 自動再送と Flow の「再試行」を合わせて合計 UPLOAD_RETRY 回まで (edge 側は冪等、protocol.md)。
            if (strcmp(save_sid, c.sid) != 0) {
                copyStr(save_sid, sizeof(save_sid), c.sid);
                save_sends = 0;
            }
            if (save_sends >= config::UPLOAD_RETRY) {
                mclog::tagWarn(kTag, "review save refused: already sent {} times for this session",
                               static_cast<unsigned>(save_sends));
                setError(str::kErrRetryExhausted);
                PhotoInfo info;
                info.status = PhotoInfo::Status::Error;
                copyStr(info.reason, sizeof(info.reason), kSaveRetryExhausted);
                publishPhoto(c.gen, info);
                break;
            }
            Reply r;
            while (save_sends < config::UPLOAD_RETRY) {
                ++save_sends;
                r = request("save", HTTP_METHOD_POST, path, body, false);
                mclog::tagInfo(kTag, "review save {}/{} -> {}", static_cast<unsigned>(save_sends),
                               static_cast<unsigned>(config::UPLOAD_RETRY), r.status);
                if (isSuccess(r.status) || (r.status >= 400 && r.status < 500)) {
                    break;  // 成功、または送り直しても変わらない 4xx
                }
                if (quit.load() || !currentGen(c.gen) || save_sends >= config::UPLOAD_RETRY) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(kSaveRetryDelayMs));
            }
            if (isSuccess(r.status)) {
                poll_active     = true;
                poll_gen        = c.gen;
                poll_started_ms = nowMs();
                poll_last_ms    = poll_started_ms;  // 最初のポーリングは 1 周期後
                copyStr(poll_sid, sizeof(poll_sid), c.sid);
            } else {
                PhotoInfo info;
                info.status = PhotoInfo::Status::Error;
                copyStr(info.reason, sizeof(info.reason), r.error_code[0] ? r.error_code : "save_failed");
                publishPhoto(c.gen, info);
            }
            break;
        }

        case CmdKind::Cancel: {
            dropPendingFrame();
            if (poll_active && strcmp(poll_sid, c.sid) == 0) {
                poll_active = false;
            }
            if (quit.load() && (failures.load() >= kOfflineAfterFailures || !wifiUp())) {
                break;  // 閉じる途中で edge も不通: 待たずに終わる (edge は 5 分で破棄する)
            }
            snprintf(path, sizeof(path), "/v1/sessions/%s/cancel", c.sid);
            sending_cancel = true;
            const Reply r  = request("cancel", HTTP_METHOD_POST, path, nullptr, !quit.load());
            sending_cancel = false;
            mclog::tagInfo(kTag, "session_cancel -> {}", r.status);
            break;
        }

        case CmdKind::Candidate: {
            bool ok = false;
            JpegBytes jpeg;
            fetchCandidate(c.sid, ok, jpeg);
            std::lock_guard<std::mutex> lock(mutex);
            if (c.seq == cand_wanted_seq) {
                cand_jpeg     = std::move(jpeg);  // 所有権は mailbox へ
                cand_ok       = ok;
                cand_done_seq = c.seq;
            }
            // Flow がもう待っていなければ、jpeg はここで解放される。
            break;
        }

        case CmdKind::Reconnect:
            dropHttp();
            failures.store(0);
            requested_once = false;  // Wi-Fi が繋がっていればすぐ hello
            setError(str::kNetReconnecting);
            mclog::tagInfo(kTag, "reconnect requested");
            break;
    }
}

void EdgeWorker::pollPhoto(uint32_t now)
{
    if (!poll_active) {
        return;
    }
    if (!currentGen(poll_gen)) {
        poll_active = false;  // 撮り直し・中止
        return;
    }
    if (now - poll_started_ms >= config::UPLOAD_WAIT_MS) {
        poll_active = false;
        PhotoInfo info;
        info.status = PhotoInfo::Status::Error;
        copyStr(info.reason, sizeof(info.reason), "timeout");
        setError(str::kNetPhotoTimeout);
        publishPhoto(poll_gen, info);
        return;
    }
    if (now - poll_last_ms < kPhotoPollIntervalMs) {
        return;
    }
    poll_last_ms = now;
    char path[96];
    snprintf(path, sizeof(path), "/v1/sessions/%s/photo", poll_sid);
    const Reply r = request("photo", HTTP_METHOD_GET, path, nullptr, true);
    if (r.status != 200) {
        return;  // 次の周期で再試行 (UPLOAD_WAIT_MS まで)
    }
    JsonDocument doc;
    if (deserializeJson(doc, static_cast<const char*>(json), json_len) != DeserializationError::Ok) {
        setErrorOp("photo", statusText(kErrBadJson));
        return;
    }
    const char* status = doc["status"] | "";
    if (strcmp(status, "pending") == 0) {
        return;
    }
    PhotoInfo info;
    poll_active = false;
    if (strcmp(status, "ready") == 0) {
        const char* photo_url  = doc["photo_url"] | "";
        const char* share_url  = doc["share_url"] | "";
        const char* expires_at = doc["expires_at"] | "";
        if (photo_url[0] == '\0' || share_url[0] == '\0' || expires_at[0] == '\0') {
            // ready なら 3 つとも必須 (protocol.md)。欠けた応答で QR を出さない。
            info.status = PhotoInfo::Status::Error;
            copyStr(info.reason, sizeof(info.reason), "bad_response");
            mclog::tagWarn(kTag, "photo ready without {}", photo_url[0] == '\0'   ? "photo_url"
                                                           : share_url[0] == '\0' ? "share_url"
                                                                                  : "expires_at");
            setErrorOp("photo", str::kNetBadJson);
        } else if (strlen(photo_url) >= sizeof(info.photo_url) || strlen(share_url) >= sizeof(info.share_url) ||
                   strlen(expires_at) >= sizeof(info.expires_at)) {
            // 切り詰めた URL の QR は出さない (spec §9「QR を捏造しない」)
            info.status = PhotoInfo::Status::Error;
            copyStr(info.reason, sizeof(info.reason), "bad_photo_url");
            setErrorOp("photo", str::kNetBadPhotoUrl);
        } else {
            info.status = PhotoInfo::Status::Ready;
            copyStr(info.photo_url, sizeof(info.photo_url), photo_url);
            copyStr(info.share_url, sizeof(info.share_url), share_url);
            copyStr(info.expires_at, sizeof(info.expires_at), expires_at);
            // URL・トークンはログに出さない (spec §9)
            mclog::tagInfo(kTag, "photo ready (expires {}) after {} ms", static_cast<const char*>(info.expires_at),
                           now - poll_started_ms);
        }
    } else {
        info.status = PhotoInfo::Status::Error;
        copyStr(info.reason, sizeof(info.reason), doc["reason"] | "error");
        mclog::tagWarn(kTag, "photo error: {}", static_cast<const char*>(info.reason));
        setErrorOp("photo", info.reason);
    }
    publishPhoto(poll_gen, info);
}

void EdgeWorker::sendHello()
{
    JsonDocument req;
    req["device_id"]        = local::kDeviceId;
    req["protocol_version"] = kProtocolVersion;
    char body[128];
    serializeJson(req, body, sizeof(body));
    const bool was_online = online();
    // hello は冪等なので、keep-alive の接続が切られていたら 1 回だけ新しい接続で送り直す。
    const Reply r = request("hello", HTTP_METHOD_POST, "/v1/hello", body, true);
    if (r.status != 200 || was_online) {
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, static_cast<const char*>(json), json_len) != DeserializationError::Ok) {
        return;
    }
    const int countdown = doc["countdown_sec"] | 0;
    const int max_faces = doc["max_faces"] | 0;
    mclog::tagInfo(kTag, "edge online: ready={} state={} max_faces={} countdown={}", (doc["ready"] | false) ? 1 : 0,
                   doc["edge_state"] | "?", max_faces, countdown);
    if (countdown != 0 && static_cast<uint32_t>(countdown) != config::COUNTDOWN_SEC) {
        mclog::tagWarn(kTag, "countdown differs: edge {} s, device {} s (device wins)", countdown,
                       config::COUNTDOWN_SEC);
    }
    if (max_faces != 0 && max_faces != config::MAX_FACES) {
        mclog::tagWarn(kTag, "max_faces differs: edge {}, device {} (screen text uses device value)", max_faces,
                       static_cast<unsigned>(config::MAX_FACES));
    }
}

void EdgeWorker::sendFrame(const FrameMeta& meta)
{
    char path[96];
    snprintf(path, sizeof(path), "/v1/sessions/%s/frames", meta.sid);
    char v_id[12], v_ms[12], v_sx[12], v_sy[12], v_w[8], v_h[8];
    snprintf(v_id, sizeof(v_id), "%lu", static_cast<unsigned long>(meta.frame_id));
    snprintf(v_ms, sizeof(v_ms), "%lu", static_cast<unsigned long>(meta.capture_ms));
    snprintf(v_sx, sizeof(v_sx), "%d", meta.servo_x);
    snprintf(v_sy, sizeof(v_sy), "%d", meta.servo_y);
    snprintf(v_w, sizeof(v_w), "%u", static_cast<unsigned>(meta.width));
    snprintf(v_h, sizeof(v_h), "%u", static_cast<unsigned>(meta.height));
    // kFrameHeaders と同じ並び。X-Format は rgb565 (カメラ層の RGB565 LE をそのまま。edge は little)。
    const Header headers[] = {
        {"X-Frame-Id", v_id}, {"X-Capture-Ms", v_ms}, {"X-Servo-X", v_sx},     {"X-Servo-Y", v_sy},
        {"X-Width", v_w},     {"X-Height", v_h},      {"X-Format", "rgb565"}, {"X-Phase", phaseName(meta.phase)},
    };
    static_assert(sizeof(headers) / sizeof(headers[0]) == sizeof(kFrameHeaders) / sizeof(kFrameHeaders[0]),
                  "frame header list mismatch");

    Request rq;
    rq.method        = HTTP_METHOD_POST;
    rq.path          = path;
    rq.content_type  = "application/octet-stream";
    rq.body          = slot[send_idx];
    rq.body_len      = meta.len;
    rq.frame_headers = headers;

    const uint32_t t0 = nowMs();
    // 失敗しても同じフレームは再送しない (protocol.md)。次のフレームを送る。
    Reply r         = exchange(rq);
    last_request_ms = nowMs();
    requested_once  = true;
    const uint32_t rtt = last_request_ms - t0;
    if (r.status < 0) {
        noteFailure("frame", r);
        return;
    }
    if (r.status != 200) {
        noteFailure("frame", r);  // 撮影中のフレームは 200 以外すべて異常 (unknown_session など)
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, static_cast<const char*>(json), json_len) != DeserializationError::Ok) {
        r.status = kErrBadJson;
        noteFailure("frame", r);
        return;
    }
    const char* sid         = doc["session_id"] | "";
    const uint32_t frame_id = doc["frame_id"] | 0UL;
    if (strcmp(sid, meta.sid) != 0 || frame_id != meta.frame_id) {
        r.status = kErrMismatch;
        noteFailure("frame", r);
        return;
    }
    noteSuccess();

    FrameResult fr;
    fr.valid             = true;
    fr.frame_id          = frame_id;
    fr.dropped           = doc["dropped"] | false;
    fr.face_count        = clampU8(doc["face_count"] | 0);
    fr.target_face_count = clampU8(doc["target_face_count"] | 0);
    fr.all_in_frame      = doc["all_in_frame"] | false;
    fr.all_eyes_open     = doc["all_eyes_open"] | false;
    fr.all_smiling       = doc["all_smiling"] | false;
    fr.servo_dx          = doc["servo_dx"] | 0;
    fr.servo_dy          = doc["servo_dy"] | 0;
    fr.hint              = parseHint(doc["hint"] | static_cast<const char*>(nullptr));
    fr.accepted          = doc["accepted"] | false;
    fr.latency_ms        = static_cast<uint16_t>(std::min<uint32_t>(doc["latency_ms"] | 0UL, 65535));

    ++stats_frames;
    stats_rtt_sum += rtt;
    stats_rtt_max = std::max(stats_rtt_max, rtt);
    stats_edge_sum += fr.latency_ms;
    mclog::tagDebug(kTag, "frame {} {}: faces {}/{} servo ({},{}) acc={} rtt {} ms edge {} ms", fr.frame_id,
                    phaseName(meta.phase), static_cast<unsigned>(fr.face_count),
                    static_cast<unsigned>(fr.target_face_count), fr.servo_dx, fr.servo_dy, fr.accepted ? 1 : 0, rtt,
                    static_cast<unsigned>(fr.latency_ms));

    // mailbox への書き込み順の約束 (mutex 下):
    //  1. 古い世代 (sessionStart / sessionCancel より前) の結果は捨てる。
    //  2. accepted=true は accepted_result に sticky に置き、スロットの保留フレームを捨てて
    //     この世代のフレーム送信を止める。edge は採用後のフレームに dropped=true を返すので、
    //     Flow が accepted を受け取る前に N+1 の応答が来ても accepted は消えない。
    //  3. dropped=true は、まだ受け取られていない dropped でない結果を上書きしない。
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (meta.gen == generation) {
            if (fr.accepted) {
                accepted_result  = fr;
                accepted_pending = true;
                frames_stopped   = true;
                has_frame        = false;
            } else if (!(fr.dropped && result_fresh && !result.dropped)) {
                result       = fr;
                result_fresh = true;
            }
        }
    }
    if (fr.accepted) {
        mclog::tagInfo(kTag, "accepted frame_id={}; stop sending frames for this session", fr.frame_id);
    }
}

void EdgeWorker::fetchCandidate(const char* sid, bool& ok, JpegBytes& jpeg)
{
    ok = false;
    char path[96];
    snprintf(path, sizeof(path), "/v1/sessions/%s/candidate", sid);
    Request rq;
    rq.method     = HTTP_METHOD_GET;
    rq.path       = path;
    rq.binary_out = &jpeg;
    Reply r;
    for (int attempt = 0; attempt < 2; ++attempt) {  // 通信失敗のときだけ 1 回送り直す (GET は冪等)
        r = exchange(rq);
        if (r.status >= 0 || r.status == kErrNoWifi || r.status == kErrAborted) break;
    }
    last_request_ms = nowMs();
    requested_once  = true;
    if (r.status != 200) {
        jpeg.clear();
        noteResponse("candidate", r);
        return;
    }
    noteSuccess();
    ok = !jpeg.empty();
    mclog::tagInfo(kTag, "candidate jpeg {} bytes", jpeg.size());
}

EdgeWorker::Reply EdgeWorker::request(const char* op, esp_http_client_method_t method, const char* path,
                                      const char* body, bool retry_transport)
{
    Request rq;
    rq.method = method;
    rq.path   = path;
    if (body != nullptr) {
        rq.content_type = "application/json";
        rq.body         = reinterpret_cast<const uint8_t*>(body);
        rq.body_len     = strlen(body);
    }
    Reply r = exchange(rq);
    if (retry_transport && r.status < 0 && r.status != kErrNoWifi && r.status != kErrAborted) {
        // keep-alive の接続が edge 側で閉じられていた場合など。新しい接続で 1 回だけ送り直す。
        mclog::tagWarn(kTag, "{}: {} ({}); retry once", op, statusText(r.status), r.status);
        r = exchange(rq);
    }
    last_request_ms = nowMs();
    requested_once  = true;
    noteResponse(op, r);
    return r;
}

bool EdgeWorker::ensureHttp()
{
    if (http != nullptr) {
        return true;
    }
    esp_http_client_config_t cfg = {};
    cfg.host                  = local::kEdgeHost;  // IP アドレス (名前解決はしない)
    cfg.port                  = local::kEdgePort;
    cfg.path                  = "/";
    cfg.transport_type        = HTTP_TRANSPORT_OVER_TCP;
    cfg.timeout_ms            = static_cast<int>(config::EDGE_TIMEOUT_MS);  // リクエストごとに armDeadline() が残り時間へ縮める
    cfg.disable_auto_redirect = true;
    cfg.buffer_size           = kHttpRxBuf;
    cfg.buffer_size_tx        = kHttpTxBuf;
    cfg.user_agent            = "stackchan-photobooth";
    cfg.keep_alive_enable     = true;  // TCP keep-alive。HTTP は 1.1 の持続接続で 1 本を使い回す (protocol.md)
    http                      = esp_http_client_init(&cfg);
    if (http == nullptr) {
        mclog::tagError(kTag, "http client init failed");
        return false;
    }
    // 認証ヘッダはクライアントが覚えるので 1 回だけ。値はログに出さない
    // (esp_http_client がヘッダを出すのは DEBUG レベルだけで、純正の既定では出ない)。
    esp_http_client_set_header(http, "X-Device-Id", local::kDeviceId);
    esp_http_client_set_header(http, "X-Device-Key", local::kSharedKey);
    http_open = false;
    return true;
}

void EdgeWorker::closeConnection()
{
    disarmWatchdog();  // fd を閉じる前に番人を止める
    if (http != nullptr) {
        esp_http_client_close(http);
    }
    http_open = false;
}

void EdgeWorker::dropHttp()
{
    disarmWatchdog();  // fd を閉じる前に番人を止める
    // 失敗した後のクライアントは使い回さず、作り直す (途中まで読んだ応答や内部状態を持ち越さない)。
    if (http != nullptr) {
        esp_http_client_cleanup(http);
        http = nullptr;
    }
    http_open = false;
}

void EdgeWorker::dropIfPeerClosed()
{
    // 待っている間に edge (uvicorn) が keep-alive の接続を閉じていたら、送る前に捨てて繋ぎ直す。
    if (http == nullptr || !http_open) {
        return;
    }
    const int fd = esp_http_client_get_socket(http);
    if (fd < 0) {
        closeConnection();
        return;
    }
    char c;
    const int n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
        return;  // 生きていて、読み残しも無い
    }
    // n == 0: 相手が閉じた。n > 0: 前の応答の読み残し。それ以外: ソケットのエラー。
    closeConnection();
}

EdgeWorker::Reply EdgeWorker::exchange(const Request& rq)
{
    Reply r = exchangeGuarded(rq);
    // どの経路で終わっても番人を止める (閉じる経路では closeConnection / dropHttp が先に止めている)。
    disarmWatchdog();
    if (watchdogFired()) {
        // 期限でソケットを shutdown した。応答が読めていても信用せず、タイムアウトとして捨てる。
        dropHttp();
        if (r.status != kErrAborted) {
            r               = Reply{};
            r.status        = kErrTimeout;
            json_len        = 0;
            json[0]         = '\0';
            if (rq.binary_out != nullptr) {
                rq.binary_out->clear();
            }
        }
    }
    return r;
}

void EdgeWorker::onDeadline(void* arg)
{
    // esp_timer タスク。close() はしない (fd の持ち主は esp_http_client)。shutdown() だけして、
    // net タスクのブロックしている送受信をエラーで戻らせる。lwIP のソケットは別タスクから呼んでよい。
    auto* w = static_cast<EdgeWorker*>(arg);
    std::lock_guard<std::mutex> lock(w->wd_mutex);
    if (!w->wd_armed) {
        return;  // もうリクエストは終わっている
    }
    w->wd_fired = true;
    if (w->wd_fd >= 0) {
        shutdown(w->wd_fd, SHUT_RDWR);
    }
}

void EdgeWorker::armWatchdog()
{
    {
        std::lock_guard<std::mutex> lock(wd_mutex);
        wd_armed = true;
        wd_fired = false;
        // keep-alive の接続を使い回すなら、送る前から fd が分かっている。
        wd_fd = http_open && http != nullptr ? esp_http_client_get_socket(http) : -1;
    }
    esp_timer_stop(wd_timer);  // 動いていなければ ESP_ERR_INVALID_STATE (無視)
    esp_timer_start_once(wd_timer, static_cast<uint64_t>(config::EDGE_TIMEOUT_MS) * 1000);
}

bool EdgeWorker::watchdogSetFd(int fd)
{
    // 接続できた後に fd を教える。接続の間に期限が来ていたら false (コールバックは fd を知らず何も
    // できなかったので、呼び出し側がタイムアウトとして打ち切る)。
    std::lock_guard<std::mutex> lock(wd_mutex);
    if (wd_fired) {
        return false;
    }
    wd_fd = fd;
    return true;
}

void EdgeWorker::disarmWatchdog()
{
    {
        std::lock_guard<std::mutex> lock(wd_mutex);
        wd_armed = false;  // wd_fired は残す (exchange() が watchdogFired() で受け取る)
        wd_fd    = -1;
    }
    if (wd_timer != nullptr) {
        esp_timer_stop(wd_timer);  // 動いていなければ ESP_ERR_INVALID_STATE (無視)
    }
}

bool EdgeWorker::watchdogFired()
{
    // 1 回だけ返す (番人を掛けずに終わる次のリクエストに持ち越さない)。
    std::lock_guard<std::mutex> lock(wd_mutex);
    const bool fired = wd_fired;
    wd_fired         = false;
    return fired;
}

EdgeWorker::Reply EdgeWorker::exchangeGuarded(const Request& rq)
{
    Reply r;
    json_len = 0;
    json[0]  = '\0';
    if (rq.binary_out != nullptr) {
        rq.binary_out->clear();
    }
    // 途中で失敗したら、クライアントごと捨てる (読み残し・内部状態を次のリクエストに持ち越さない)。
    auto fail = [this, &r](int status) {
        dropHttp();
        r.status = status;
        return r;
    };
    if (quit.load() && !sending_cancel) {
        r.status = kErrAborted;
        return r;
    }
    if (!wifiUp()) {
        r.status = kErrNoWifi;
        return r;
    }
    if (!ensureHttp()) {
        r.status = kErrInternal;
        return r;
    }
    dropIfPeerClosed();

    esp_http_client_set_method(http, rq.method);
    if (esp_http_client_set_url(http, rq.path) != ESP_OK) {  // パスだけ差し替える (host / port は init のまま)
        dropHttp();
        r.status = kErrInternal;
        return r;
    }
    // esp_http_client はヘッダをハンドルに覚え続ける。リクエストごとのヘッダ (Content-Type と frame 専用の
    // X-Frame-Id など) は毎回いったん全部消してから付け直し、frame のヘッダが後続の hello / timeout /
    // review / photo / candidate に漏れないようにする。残すのは認証ヘッダ (ensureHttp) と、
    // クライアントが毎回設定し直す Host / User-Agent / Content-Length だけ。
    esp_http_client_delete_header(http, "Content-Type");
    for (const char* name : kFrameHeaders) {
        esp_http_client_delete_header(http, name);
    }
    if (rq.content_type != nullptr) {
        esp_http_client_set_header(http, "Content-Type", rq.content_type);
    }
    if (rq.frame_headers != nullptr) {
        for (size_t i = 0; i < sizeof(kFrameHeaders) / sizeof(kFrameHeaders[0]); ++i) {
            esp_http_client_set_header(http, rq.frame_headers[i].name, rq.frame_headers[i].value);
        }
    }

    // 接続 (まだなら) + リクエスト行とヘッダの送信。Content-Length は esp_http_client が付ける。
    // ここから応答本文を読み終えるまでを 1 つの期限 (EDGE_TIMEOUT_MS) で縛る (protocol.md)。
    const bool was_open = http_open;
    request_started_ms  = nowMs();
    armWatchdog();  // 絶対期限の番人 (armDeadline() が先に効くのが普通で、こちらは最後の砦)
    armDeadline();  // 接続は設定されたタイムアウトを 1 回だけ使う
    const esp_err_t err = esp_http_client_open(http, static_cast<int>(rq.body_len));
    if (err != ESP_OK) {
        r.status = err == ESP_ERR_HTTP_CONNECT ? kErrConnect : kErrSend;
        dropHttp();
        return r;
    }
    http_open = true;
    if (!watchdogSetFd(esp_http_client_get_socket(http))) {
        return fail(kErrTimeout);
    }
    if (!was_open) {
        // 本文の最後の端数セグメントが Nagle で遅れないようにする。
        const int fd  = esp_http_client_get_socket(http);
        const int one = 1;
        if (fd >= 0) {
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
    }

    // 本文。大きいフレームは区切って書き、合間に期限と終了要求を見る。
    size_t sent = 0;
    while (sent < rq.body_len) {
        if (quit.load() && !sending_cancel) {
            return fail(kErrAborted);
        }
        if (!armDeadline()) {
            return fail(kErrTimeout);
        }
        const int chunk = static_cast<int>(std::min(rq.body_len - sent, kWriteChunk));
        const int n     = esp_http_client_write(http, reinterpret_cast<const char*>(rq.body + sent), chunk);
        if (n <= 0) {
            // 書き込みの待ちが期限に達したのか、接続が切れたのかを分ける。
            return fail(armDeadline() ? kErrSend : kErrTimeout);
        }
        sent += static_cast<size_t>(n);
    }

    if (!armDeadline()) {
        return fail(kErrTimeout);
    }
    const int64_t content_len = esp_http_client_fetch_headers(http);
    if (content_len < 0) {
        return fail(content_len == -ESP_ERR_HTTP_EAGAIN || !armDeadline() ? kErrTimeout : kErrLost);
    }
    const int status = esp_http_client_get_status_code(http);

    if (rq.binary_out != nullptr && status == 200) {
        // 候補 JPEG。長さが分かっていれば先に上限を見る。chunked (長さ不明) なら読みながら見る。
        const int rc = readBinary(*rq.binary_out, content_len);
        if (rc != 0) {
            rq.binary_out->clear();
            return fail(rc);
        }
    } else {
        // JSON。長さが分かっていれば先に上限を見る。chunked なら readBody() が読みながら見る。
        if (content_len > static_cast<int64_t>(kMaxJsonBody)) {
            return fail(kErrBodyTooLarge);
        }
        const int rc   = readBody(json, kMaxJsonBody, json_len);
        json[json_len] = '\0';
        if (rc != 0) {
            json_len = 0;
            json[0]  = '\0';
            return fail(rc);
        }
        if (!isSuccess(status)) {
            JsonDocument doc;
            if (deserializeJson(doc, static_cast<const char*>(json), json_len) == DeserializationError::Ok) {
                copyStr(r.error_code, sizeof(r.error_code), doc["error"] | "");
            }
        }
    }
    // 応答を読み切ったので接続は残す。edge が Connection: close を返したときだけ閉じる。
    if (!esp_http_client_is_persistent_connection(http)) {
        closeConnection();
    }
    r.status = status;
    return r;
}

bool EdgeWorker::armDeadline()
{
    // リクエスト全体 (接続 + 送信 + ヘッダ + 本文) の期限までの残りを、次のブロッキング呼び出しの
    // タイムアウトにする。期限を過ぎていれば false (呼び出し側はタイムアウトとして打ち切る)。
    const uint32_t elapsed = nowMs() - request_started_ms;
    if (elapsed >= config::EDGE_TIMEOUT_MS) {
        return false;
    }
    const int remaining = static_cast<int>(config::EDGE_TIMEOUT_MS - elapsed);
    esp_http_client_set_timeout_ms(http, remaining);  // 1 ms 以上。期限を越えて待たない
    return true;
}

EdgeWorker::ReadStep EdgeWorker::readSome(char* dst, size_t want, size_t& n, int& err)
{
    // 応答本文を 1 回 (最大 kReadChunk) 読む。前に期限と終了要求を見る。
    n = 0;
    if (quit.load() && !sending_cancel) {
        err = kErrAborted;
        return ReadStep::Error;
    }
    if (!armDeadline()) {
        err = kErrTimeout;
        return ReadStep::Error;
    }
    const int r = esp_http_client_read(http, dst, static_cast<int>(std::min(want, kReadChunk)));
    if (r == -ESP_ERR_HTTP_EAGAIN) {
        return ReadStep::Again;  // この読み出しの待ちが切れた。期限は次の armDeadline() で判定する
    }
    if (r < 0) {
        err = kErrLost;
        return ReadStep::Error;
    }
    if (r == 0) {
        return ReadStep::End;  // 読み切った、または相手が閉じた
    }
    n = static_cast<size_t>(r);
    return ReadStep::Data;
}

int EdgeWorker::readBody(char* dst, size_t cap, size_t& got)
{
    // 固定の領域 (cap バイト) に読む。Content-Length の無い chunked 応答でも、cap を超えた時点で
    // kErrBodyTooLarge にする (cap ちょうどで終わる応答は通す)。戻り値は 0 (読み切った) か kErr*。
    got = 0;
    for (;;) {
        char extra;  // cap まで読んだ後、まだ続きがあるかを確かめる 1 バイト
        const bool full = got >= cap;
        size_t n        = 0;
        int err         = 0;
        const ReadStep step = full ? readSome(&extra, 1, n, err) : readSome(dst + got, cap - got, n, err);
        if (step == ReadStep::Again) continue;
        if (step == ReadStep::Error) return err;
        if (step == ReadStep::End) break;
        if (full) return kErrBodyTooLarge;
        got += n;
    }
    return esp_http_client_is_complete_data_received(http) ? 0 : kErrLost;
}

int EdgeWorker::readBinary(JpegBytes& out, int64_t content_len)
{
    // PSRAM のバッファに読む。content_len > 0 ならその大きさで 1 回だけ確保する。
    // 0 (chunked / 長さ不明) なら kBinaryInitialBytes から倍々に伸ばし、合計が kMaxCandidateBytes を
    // 超えた時点で kErrBodyTooLarge にする。
    if (content_len > static_cast<int64_t>(kMaxCandidateBytes)) {
        return kErrBodyTooLarge;
    }
    const bool known = content_len > 0;
    size_t got       = 0;
    try {
        out.resize(known ? static_cast<size_t>(content_len) : kBinaryInitialBytes);
        for (;;) {
            if (got >= out.size() && !known && out.size() < kMaxCandidateBytes) {
                out.resize(std::min(out.size() * 2, kMaxCandidateBytes));
            }
            char extra;  // 上限 (または宣言された長さ) まで読んだ後、まだ続きがあるかを確かめる 1 バイト
            const bool full = got >= out.size();
            size_t n        = 0;
            int err         = 0;
            const ReadStep step = full ? readSome(&extra, 1, n, err)
                                       : readSome(reinterpret_cast<char*>(out.data()) + got, out.size() - got, n, err);
            if (step == ReadStep::Again) continue;
            if (step == ReadStep::Error) return err;
            if (step == ReadStep::End) break;
            if (full) return kErrBodyTooLarge;
            got += n;
        }
        out.resize(got);
    } catch (const std::bad_alloc&) {
        mclog::tagError(kTag, "candidate alloc failed (free PSRAM {})", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return kErrInternal;
    }
    if (got == 0 || (known && got != static_cast<size_t>(content_len)) ||
        !esp_http_client_is_complete_data_received(http)) {
        return kErrLost;
    }
    return 0;
}

void EdgeWorker::noteResponse(const char* op, const Reply& r)
{
    if (r.status == kErrAborted) {
        return;  // アプリを閉じる途中。失敗に数えない
    }
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
    mclog::tagWarn(kTag, "{}: http {} {}", op, r.status, static_cast<const char*>(r.error_code));
    char text[64];
    snprintf(text, sizeof(text), "HTTP %d %s", r.status, r.error_code);
    setErrorOp(op, text);
}

void EdgeWorker::noteSuccess()
{
    last_ok_ms.store(nowMs());
    ever_ok.store(true);
    if (failures.exchange(0) >= kOfflineAfterFailures) {
        mclog::tagInfo(kTag, "edge reachable again");
    }
}

void EdgeWorker::noteFailure(const char* op, const Reply& r)
{
    if (r.status == kErrAborted) {
        return;
    }
    const uint8_t prev = failures.load();
    const uint8_t n    = prev < 255 ? prev + 1 : prev;
    failures.store(n);
    ++stats_failures;
    const char* text = statusText(r.status);
    if (r.status >= 0 && text[0] == '\0') {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP %d %s", r.status, r.error_code);
        setErrorOp(op, buf);
    } else {
        setErrorOp(op, text);
    }
    mclog::tagWarn(kTag, "{} failed: status {} {} ({} in a row)", op, r.status, static_cast<const char*>(r.error_code),
                   static_cast<unsigned>(n));
    if (prev < kOfflineAfterFailures && n >= kOfflineAfterFailures) {
        mclog::tagWarn(kTag, "edge offline after {} failures", static_cast<unsigned>(n));
    }
}

void EdgeWorker::logStats(uint32_t now)
{
    const uint32_t dt = now - stats_since_ms;
    if (dt < kStatsIntervalMs) {
        return;
    }
    if (stats_frames > 0 || stats_failures > 0 || stats_skipped > 0) {
        const float fps         = stats_frames * 1000.0f / dt;
        const uint32_t rtt_avg  = stats_frames ? stats_rtt_sum / stats_frames : 0;
        const uint32_t edge_avg = stats_frames ? stats_edge_sum / stats_frames : 0;
        mclog::tagInfo(kTag,
                       "send {} frames in {} ms ({:.1f} fps), rtt avg {} max {} ms, edge avg {} ms, failures {}, "
                       "skipped {}, stack free {}",
                       stats_frames, dt, fps, rtt_avg, stats_rtt_max, edge_avg, stats_failures, stats_skipped,
                       uxTaskGetStackHighWaterMark(nullptr));
    }
    stats_since_ms = now;
    stats_frames   = 0;
    stats_failures = 0;
    stats_skipped  = 0;
    stats_rtt_sum  = 0;
    stats_rtt_max  = 0;
    stats_edge_sum = 0;
}

}  // namespace photobooth::net

#endif  // PHOTOBOOTH_EDGE_ENABLED
