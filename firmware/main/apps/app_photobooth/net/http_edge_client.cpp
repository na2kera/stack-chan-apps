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
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_tls_errors.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal/hal.h>
#include <mbedtls/x509.h>
#include <mooncake_log.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <new>
#include <utility>

#include "../view/strings.h"
#include "edge_config.h"
#include "edge_parse.h"
#include "edge_url.h"
#include "frame_jpeg.h"
#include "frame_stats.h"
#include "link_logic.h"
#include "network.h"

namespace photobooth::net {

namespace {

namespace local = config::local;

constexpr const char* kTag = "PB-Edge";

constexpr int kProtocolVersion = 1;
// esp_http_client + mbedTLS (https) + ArduinoJson + fmt の分。高水位は logStats() で出す。
// 6b の初期値 (8192 → 12288)。実機の high-water で最小余裕 2 KiB 以上を保てる最小値に決め直す
// (docs/design/step6-cloud-device.md §3.2「メモリ」)。
constexpr uint32_t kTaskStack   = 12288;
constexpr UBaseType_t kTaskPrio = 3;  // メインループ (1) より上、Wi-Fi / lwIP より下
constexpr UBaseType_t kQueueDepth = 8;
// この回数続けて失敗したら offline にしてフレームを送らず、hello で復帰を待つ。
constexpr uint8_t kOfflineAfterFailures = link::kOfflineAfterFailures;
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
constexpr uint32_t kClockPollMs         = 100;  // 時刻の同期を待つ間の確認間隔
constexpr int kHttpRxBuf                = 1024;
constexpr int kHttpTxBuf                = 1024;  // 1 行目 (メソッド + パス) とヘッダ 1 本ずつが入る大きさ
constexpr size_t kWriteChunk            = 8192;  // フレーム本文を書く単位 (合間に期限と終了要求を見る)
constexpr size_t kReadChunk             = 4096;  // 応答本文を読む単位 (同上)

// QVGA RGB565 1 枚分。これより大きいフレームは offerFrame() で断る。
constexpr size_t kSlotBytes = 320 * 240 * 2;

// 送る形式 (docs/design/step6-cloud-device.md §3.3)。切り替えは config.h の FRAME_FORMAT_JPEG。
const frame::Format kFrameFormat = frame::selectFormat(config::FRAME_FORMAT_JPEG);

// 通信の失敗を表す負の status (HTTP の status は正)。
constexpr int kErrNoWifi       = -100;
constexpr int kErrBodyTooLarge = -101;
constexpr int kErrBadJson      = -102;
constexpr int kErrMismatch     = -103;
constexpr int kErrConnect      = -110;  // TCP 接続できない (http の分類できない失敗も)
constexpr int kErrSend         = -111;  // ヘッダ・本文を送れない
constexpr int kErrLost         = -112;  // 応答の途中で切れた
constexpr int kErrTimeout      = -113;  // 応答が時間内に来ない
constexpr int kErrInternal     = -114;  // クライアントを作れない (メモリ不足など)
constexpr int kErrAborted      = -115;  // アプリを閉じる途中
constexpr int kErrDns          = -116;  // 名前解決できない
constexpr int kErrCert         = -117;  // 証明書を検証できない
constexpr int kErrClock        = -118;  // 証明書の期限の検査で落ちた (こちらの時刻が不正)
constexpr int kErrTls          = -119;  // その他の TLS の失敗

// ロジック層 (link_logic.h) が持っている esp-tls / mbedTLS の値の写しが本物と一致すること。
static_assert(link::kTlsErrCannotResolveHostname == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME, "esp-tls code");
static_assert(link::kTlsErrCannotCreateSocket == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET, "esp-tls code");
static_assert(link::kTlsErrFailedConnectToHost == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST, "esp-tls code");
static_assert(link::kTlsErrConnectionTimeout == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT, "esp-tls code");
static_assert(link::kCertFlagExpired == MBEDTLS_X509_BADCERT_EXPIRED, "mbedtls flag");
static_assert(link::kCertFlagFuture == MBEDTLS_X509_BADCERT_FUTURE, "mbedtls flag");

static_assert(config::UPLOAD_RETRY >= 1, "UPLOAD_RETRY must be >= 1");

// frame リクエストだけに付けるヘッダ (protocol.md)。他のリクエストの前に消す。
constexpr const char* kFrameHeaders[] = {
    "X-Frame-Id", "X-Capture-Ms", "X-Servo-X", "X-Servo-Y", "X-Width", "X-Height", "X-Format", "X-Phase",
};

uint32_t nowMs()
{
    return GetHAL().millis();  // Flow・カメラ層と同じ時計 (画面には触らない HAL 呼び出し)
}

void copyStr(char* dst, size_t n, const char* src)
{
    snprintf(dst, n, "%s", src != nullptr ? src : "");
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
        case kErrDns:
            return str::kNetDnsFailed;
        case kErrCert:
            return str::kNetCertError;
        case kErrClock:
            return str::kNetClockNotSynced;
        case kErrTls:
            return str::kNetTlsFailed;
        case 401:
            return str::kNetUnauthorized;
        default:
            return status < 0 ? str::kNetError : "";
    }
}

bool isSuccess(int status)
{
    return status >= 200 && status < 300;
}

bool wifiUp()
{
    return Network::status() == Network::Status::Connected;
}

bool clockValidNow()
{
    return link::clockValid(static_cast<int64_t>(time(nullptr)));
}

// config_local.h の EDGE_BASE_URL を 1 回だけ分解して覚える (書式が不正なら status != Ok)。
struct ParsedUrl {
    EdgeUrl url;
    UrlStatus status;
};

const ParsedUrl& edgeUrl()
{
    static const ParsedUrl parsed = [] {
        ParsedUrl p{};
        p.status = parseEdgeUrl(local::kEdgeBaseUrl, p.url);
        return p;
    }();
    return parsed;
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
        uint32_t timeout_ms;  // Candidate: リクエスト全体の期限 (0 なら EDGE_TIMEOUT_MS)
        bool allow_retry;     // Candidate: 通信失敗のとき 1 回だけ送り直すか
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
        int status            = 0;
        char error_code[40]   = {};     // エラー応答の {"error": ...}
        bool connected        = false;  // esp_http_client_open() が成功した (接続と要求の送信まで済んだ)
        bool response_timeout = false;  // 接続後、応答 (ヘッダ・本文) の待ちで期限切れ
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
    bool encodeJpeg(const FrameMeta& meta, uint8_t*& out, size_t& out_len);
    void sendHello();
    void pollPhoto(uint32_t now);
    void publishPhoto(uint32_t gen, const PhotoInfo& info);
    void fetchCandidate(const char* sid, uint32_t timeout_ms, bool allow_retry, bool& ok, JpegBytes& jpeg);
    // JSON / 空本文のリクエスト。応答本文は json (NUL 終端、json_len バイト) に入る。
    // retry_transport なら通信失敗 (status < 0) のとき接続を作り直して 1 回だけ送り直す
    // (keep-alive の接続が edge 側で閉じられていた場合の対策。冪等なリクエストだけ)。
    Reply request(const char* op, esp_http_client_method_t method, const char* path, const char* body,
                  bool retry_transport, link::RequestKind kind = link::RequestKind::Other);
    int classifyOpenError();
    void waitForClock();
    bool commandsWaiting() const;
    Reply exchange(const Request& rq);
    bool ensureHttp();
    bool armDeadline();
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
    std::atomic<bool> starting{false};  // 直近の hello が「準備中」(接続後の応答待ちで期限切れ、または 5xx)
    std::atomic<bool> hello_settled{false};  // 最初の hello の結果 (打ち切り以外) が出た
    std::atomic<bool> latency_sensitive{false};  // hello を止める段階 (Flow が setLatencySensitive で知らせる)

    // ---- begin() で決めて以後は読むだけ ----
    EdgeUrl url{};

    // ---- net タスクだけが触る ----
    esp_http_client_handle_t http = nullptr;
    bool http_open                = false;  // TCP 接続を持っている (と思っている)
    bool sending_cancel           = false;  // 終了要求の後でも送ってよいリクエスト (session_cancel) の最中
    uint32_t request_started_ms   = 0;      // 今のリクエストの期限の起点 (exchange の中だけで使う)
    uint32_t request_timeout_ms   = config::EDGE_TIMEOUT_MS;  // 今のリクエストの期限 (exchange の中だけで使う)
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
    // 送信 fps・往復時間・JPEG の符号化 (logStats で出してリセット)
    uint32_t stats_since_ms = 0;
    uint32_t stats_failures = 0;  // 全リクエストの失敗 (frame 以外も含む)
    frame::Stats frame_stats;     // frame の送信と符号化
    uint32_t encode_seq = 0;      // 符号化を試みた通し番号 (失敗の注入用)
    // hello の統計 (同上)。rtt は再送を含めた 1 回の hello の時間
    uint32_t stats_hello          = 0;
    uint32_t stats_hello_ok       = 0;
    uint32_t stats_hello_starting = 0;
    uint32_t stats_hello_rtt_sum  = 0;
    uint32_t stats_hello_rtt_max  = 0;
    bool clock_checked            = false;  // https の最初の hello の前の時刻確認を済ませた
    // 接続の統計 (同上)。https で毎回ハンドシェイクしていないかを実機で確かめる
    uint32_t stats_conn_new    = 0;  // 新しく接続した (TCP + https なら TLS ハンドシェイク)
    uint32_t stats_conn_reused = 0;  // keep-alive の接続を使い回した
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
    const ParsedUrl& pu = edgeUrl();
    if (pu.status != UrlStatus::Ok) {
        // URL 自体はログに出さない (userinfo などを含みうる)。理由の名前だけ出す。
        mclog::tagError(kTag, "EDGE_BASE_URL is invalid ({}); edge disabled", urlStatusName(pu.status));
        copyStr(begin_error_, sizeof(begin_error_), str::kNetBadUrl);
        return false;
    }
    auto w = std::make_shared<EdgeWorker>();
    w->url = pu.url;
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
    // 接続先はログに出さない (URL を出さない方針)。scheme とポートだけ出す。
    mclog::tagInfo(kTag, "net task started (stack {}, {} port {})", kTaskStack, pu.url.https ? "https" : "http",
                   static_cast<unsigned>(pu.url.port));
    if (config::JPEG_FAIL_EVERY != 0) {
        mclog::tagWarn(kTag, "JPEG fail injection every {} (TEST BUILD)", config::JPEG_FAIL_EVERY);
    }
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
    if (!worker_) {
        return LinkState::Offline;
    }
    const auto& w = *worker_;
    link::LinkSnapshot snap;
    // hello_settled を最初に読む。net タスクは hello の結果 (noteSuccess() / noteFailure() による ever_ok・
    // failures・last_ok_ms と starting) を書いた後に hello_settled を立てるので、true を読めたなら以下で
    // 読む状態はその結果以降のもの。逆順だと「古い ever_ok=false と新しい hello_settled=true」を組み合わせて
    // Offline と判定し、最初の hello の成功直後のタッチで診断画面に入りうる。
    snap.hello_settled = w.hello_settled.load(std::memory_order_acquire);
    snap.ever_ok    = w.ever_ok.load();
    snap.failures   = w.failures.load();
    snap.last_ok_ms = w.last_ok_ms.load();  // now より先に読む (EdgeWorker::online() と同じ理由)
    snap.starting   = w.starting.load();
    snap.latency_sensitive = w.latency_sensitive.load();
    return link::linkState(snap, wifiUp(), nowMs(), kOnlineWindowMs);
}

void HttpEdgeClient::setLatencySensitive(bool active)
{
    if (worker_) {
        worker_->latency_sensitive.store(active);
    }
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

void HttpEdgeClient::requestCandidate(const Session& s, uint32_t timeout_ms, bool allow_retry)
{
    cand_waiting_      = true;
    cand_requested_ms_ = nowMs();
    cand_timeout_ms_   = timeout_ms != 0 ? timeout_ms : config::EDGE_TIMEOUT_MS;
    if (!worker_) return;  // pollCandidate() がすぐ失敗を返す
    auto& w = *worker_;
    if (++cand_seq_ == 0) ++cand_seq_;  // 0 は「待っていない」
    EdgeWorker::Command c{};
    c.kind = EdgeWorker::CmdKind::Candidate;
    c.seq         = cand_seq_;
    c.timeout_ms  = cand_timeout_ms_;
    c.allow_retry = allow_retry;
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
        } else if (waited >= cand_timeout_ms_ + kCandidateWaitMarginMs) {  // 従来どおり「期限 + 余裕」。REVIEW は 3.5 秒、SHUTTER は 1.3 秒
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
    const ParsedUrl& pu = edgeUrl();
    if (pu.status == UrlStatus::Ok) {
        formatEdgeUrl(pu.url, out.edge_url, sizeof(out.edge_url));  // path と鍵は含まない
    }
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
    return wake != nullptr && queue != nullptr && slot[0] != nullptr && slot[1] != nullptr;
}

EdgeWorker::~EdgeWorker()
{
    // 最後の shared_ptr を手放した側 (通常は end()、切り離し時は net タスク) で走る。
    // この時点で net タスクは run() を抜けているので、http はもう使われていない。
    dropHttp();
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
    link::LinkSnapshot snap;
    snap.ever_ok  = ever_ok.load();
    snap.failures = failures.load();
    snap.latency_sensitive = latency_sensitive.load();
    // last_ok を先に読んでから now を取る (逆だと net タスクの更新で差が負になりうる)。
    snap.last_ok_ms = last_ok_ms.load();
    return link::linkOnline(snap, nowMs(), kOnlineWindowMs);
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
                frame_stats.addSkipped();
            }
        }

        // (4) 何も送っていない間の接続確認 (offline なら復帰の確認)。HELLO_INTERVAL_MS ごと。
        // 撮影〜写真の準備完了の間と依頼が残っているときは送らない (hello は最長 HELLO_TIMEOUT_MS 塞ぐため)。
        link::HelloGate gate;
        gate.wifi_up          = wifi_up;
        gate.quitting         = quit.load();
        gate.latency_sensitive   = latency_sensitive.load();
        gate.commands_waiting = commandsWaiting();
        gate.requested_once   = requested_once;
        gate.since_last_ms    = nowMs() - last_request_ms;
        gate.interval_ms      = config::HELLO_INTERVAL_MS;
        if (link::shouldSendHello(gate)) {
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

bool EdgeWorker::commandsWaiting() const
{
    return uxQueueMessagesWaiting(queue) > 0;
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
            fetchCandidate(c.sid, c.timeout_ms, c.allow_retry, ok, jpeg);
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
    PhotoInfo info;
    const char* missing = "";
    const ParseStatus ps = parsePhotoInfo(static_cast<const char*>(json), json_len, info, &missing);
    if (ps == ParseStatus::BadJson) {
        setErrorOp("photo", statusText(kErrBadJson));
        return;
    }
    if (ps == ParseStatus::Ok && info.status == PhotoInfo::Status::Pending) {
        return;
    }
    poll_active = false;
    if (ps == ParseStatus::BadResponse) {
        // ready なら 3 つとも必須 (protocol.md)。欠けた応答で QR を出さない。
        mclog::tagWarn(kTag, "photo ready without {}", missing);
        setErrorOp("photo", str::kNetBadJson);
    } else if (ps == ParseStatus::BadPhotoUrl) {
        // 切り詰めた URL の QR は出さない (spec §9「QR を捏造しない」)
        setErrorOp("photo", str::kNetBadPhotoUrl);
    } else if (info.status == PhotoInfo::Status::Ready) {
        // URL・トークンはログに出さない (spec §9)
        mclog::tagInfo(kTag, "photo ready (expires {}) after {} ms", static_cast<const char*>(info.expires_at),
                       now - poll_started_ms);
    } else {
        mclog::tagWarn(kTag, "photo error: {}", static_cast<const char*>(info.reason));
        setErrorOp("photo", info.reason);
    }
    publishPhoto(poll_gen, info);
}

void EdgeWorker::waitForClock()
{
    // https の証明書の期限の検査にはシステム時刻が要る。純正は Wi-Fi 接続後に SNTP を始めるが
    // 同期を待たないので、時刻が不正 (RTC が戻っていない) なら最大 kClockWaitMs だけ待つ。
    // 待ち切れなければそのまま hello を試す (失敗すれば「時刻未同期」と出る)。
    const uint32_t t0 = nowMs();
    while (!clockValidNow() && !quit.load() && nowMs() - t0 < link::kClockWaitMs) {
        xSemaphoreTake(wake, pdMS_TO_TICKS(kClockPollMs));  // 終了要求で起こされたら抜ける
    }
    mclog::tagInfo(kTag, "clock {} after waiting {} ms", clockValidNow() ? "synced" : "still invalid",
                   nowMs() - t0);
}

void EdgeWorker::sendHello()
{
    if (!clock_checked) {
        clock_checked = true;
        if (link::shouldWaitForClock(url.https, clockValidNow())) {
            waitForClock();
            if (quit.load()) {
                return;
            }
        }
    }
    JsonDocument req;
    req["device_id"]        = local::kDeviceId;
    req["protocol_version"] = kProtocolVersion;
    char body[128];
    serializeJson(req, body, sizeof(body));
    const bool was_online = online();
    // hello は冪等なので、keep-alive の接続が切られていたら 1 回だけ新しい接続で送り直す。
    const uint32_t t0 = nowMs();
    const Reply r     = request("hello", HTTP_METHOD_POST, "/v1/hello", body, true, link::RequestKind::Hello);
    const uint32_t rtt = nowMs() - t0;

    // 接続後のどの段階で失敗したか (Reply に保持した) から「準備中」かを決める。
    link::ReplyFacts facts;
    facts.http_status      = r.status > 0 ? r.status : 0;
    facts.aborted          = r.status == kErrAborted;
    facts.connected        = r.connected;
    facts.response_timeout = r.response_timeout;
    const link::ReplyClass cls = link::classifyReply(facts);
    const bool was_starting    = starting.load();
    const bool now_starting    = link::startingAfterHello(was_starting, cls);
    starting.store(now_starting);
    // hello_settled は結果の状態 (request() の中の noteResponse() と上の starting) を書いた後に立てる
    // (読み側の HttpEdgeClient::linkState() はこれを最初に読む)。
    if (cls != link::ReplyClass::Aborted && !hello_settled.exchange(true, std::memory_order_acq_rel)) {
        mclog::tagInfo(kTag, "first hello settled (status {}, {} ms)", r.status, rtt);
    }
    if (cls != link::ReplyClass::Aborted) {
        ++stats_hello;
        stats_hello_rtt_sum += rtt;
        stats_hello_rtt_max = std::max(stats_hello_rtt_max, rtt);
        if (cls == link::ReplyClass::Success) ++stats_hello_ok;
        if (cls == link::ReplyClass::Starting) ++stats_hello_starting;
    }
    if (cls == link::ReplyClass::Starting && r.status < 0) {
        setErrorOp("hello", str::kNetStartingWait);  // 5xx は noteResponse() の「HTTP 503 ...」のまま
    }
    if (now_starting != was_starting) {
        mclog::tagInfo(kTag, "edge {} (hello status {}, {} ms)", now_starting ? "starting" : "not starting",
                       r.status, rtt);
    }
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
    // kFrameHeaders と同じ並び。X-Format は jpeg か rgb565 (カメラ層の RGB565 LE をそのまま。edge は little)。
    // X-Width / X-Height は JPEG でも元の寸法 (320 / 240)。edge は実 JPEG の寸法と照合する。
    const Header headers[] = {
        {"X-Frame-Id", v_id},
        {"X-Capture-Ms", v_ms},
        {"X-Servo-X", v_sx},
        {"X-Servo-Y", v_sy},
        {"X-Width", v_w},
        {"X-Height", v_h},
        {"X-Format", frame::formatHeader(kFrameFormat)},
        {"X-Phase", phaseName(meta.phase)},
    };
    static_assert(sizeof(headers) / sizeof(headers[0]) == sizeof(kFrameHeaders) / sizeof(kFrameHeaders[0]),
                  "frame header list mismatch");

    // JPEG の出力バッファ。image_to_jpeg() が malloc で確保したものを、送信が終わるまでこの関数が所有し、
    // 抜けるときに free() する (別のバッファへはコピーしない。§3.3)。
    struct MallocBuf {
        uint8_t* p = nullptr;
        size_t n   = 0;
        MallocBuf() = default;
        MallocBuf(const MallocBuf&) = delete;
        MallocBuf& operator=(const MallocBuf&) = delete;
        ~MallocBuf()
        {
            free(p);
        }
    } jpeg;

    Request rq;
    rq.method        = HTTP_METHOD_POST;
    rq.path          = path;
    rq.frame_headers = headers;
    // 形式は X-Format で伝える (edge は Content-Type を見ない。疑似デバイスと同じ値)
    rq.content_type  = "application/octet-stream";
    if (kFrameFormat == frame::Format::Jpeg) {
        if (!encodeJpeg(meta, jpeg.p, jpeg.n)) {
            return;  // 破棄 (RGB565 に落とさない)。回数は logStats() にまとめて出す
        }
        rq.body     = jpeg.p;
        rq.body_len = jpeg.n;  // Content-Length は返ってきた長さ
    } else {
        rq.body     = slot[send_idx];
        rq.body_len = meta.len;
    }

    // 送ったが失敗したフレームを数える (アプリを閉じる途中の打ち切りは数えない。noteFailure と同じ)
    auto fail = [this](const Reply& r) {
        if (r.status != kErrAborted) {
            frame_stats.addSendFailure();
        }
        noteFailure("frame", r);
    };

    const uint32_t t0 = nowMs();
    // 失敗しても同じフレームは再送しない (protocol.md)。次のフレームを送る。
    Reply r         = exchange(rq);
    last_request_ms = nowMs();
    requested_once  = true;
    const uint32_t rtt = last_request_ms - t0;
    if (r.status < 0) {
        fail(r);
        return;
    }
    if (r.status != 200) {
        fail(r);  // 撮影中のフレームは 200 以外すべて異常 (unknown_session など)
        return;
    }
    FrameResult fr;
    const ParseStatus ps = parseFrameResult(static_cast<const char*>(json), json_len, meta.sid, meta.frame_id, fr);
    if (ps == ParseStatus::BadJson) {
        r.status = kErrBadJson;
        fail(r);
        return;
    }
    if (ps != ParseStatus::Ok) {  // Mismatch
        r.status = kErrMismatch;
        fail(r);
        return;
    }
    noteSuccess();

    frame_stats.addSent(rtt, fr.latency_ms);
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

// send 面 (net タスクの所有) の RGB565 LE を JPEG にする。成功すれば out に image_to_jpeg() が malloc した
// バッファが入り、呼び出し側が free() する。失敗 (確保失敗を含む) は統計に数えて false。ログは 1 回ごとには出さない。
//
// エンコーダは同時に 1 つだけ動く: 呼ぶのはこの net タスク (アプリごとに 1 つ) だけで、符号化と送信は直列。
// これで PSRAM のピーク (YUYV 入力 150 KiB + 出力 約 177 KiB) が 1 組に収まる (§3.3「並列性」)。
// 入力形式と確保先は frame_jpeg.cpp。
bool EdgeWorker::encodeJpeg(const FrameMeta& meta, uint8_t*& out, size_t& out_len)
{
    out     = nullptr;
    out_len = 0;
    ++encode_seq;
    const uint32_t t0 = nowMs();
    bool ok           = false;
    if (!frame::injectEncodeFailure(encode_seq, config::JPEG_FAIL_EVERY)) {
        ok = encodeRgb565Jpeg(slot[send_idx], meta.len, meta.width, meta.height, config::FRAME_JPEG_QUALITY, &out,
                              &out_len);
    }
    const uint32_t encode_ms = nowMs() - t0;
    if (!ok) {
        frame_stats.addEncodeFailure();
        return false;
    }
    frame_stats.addEncoded(encode_ms, out_len);
    return true;
}

void EdgeWorker::fetchCandidate(const char* sid, uint32_t timeout_ms, bool allow_retry, bool& ok, JpegBytes& jpeg)
{
    ok = false;
    char path[96];
    snprintf(path, sizeof(path), "/v1/sessions/%s/candidate", sid);
    Request rq;
    rq.method     = HTTP_METHOD_GET;
    rq.path       = path;
    rq.binary_out = &jpeg;
    Reply r;
    // SHUTTER の依頼は短い期限で 1 回だけ (後ろに並ぶ save を待たせない)。REVIEW は通信失敗のときだけ
    // 1 回送り直す (GET は冪等)。
    request_timeout_ms   = link::requestTimeoutMs(link::RequestKind::Candidate, timeout_ms);
    const int attempts   = allow_retry ? 2 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        r = exchange(rq);
        if (r.status >= 0 || r.status == kErrNoWifi || r.status == kErrAborted) break;
    }
    request_timeout_ms = config::EDGE_TIMEOUT_MS;  // 以後のリクエストは既定の期限
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
                                      const char* body, bool retry_transport, link::RequestKind kind)
{
    request_timeout_ms = link::requestTimeoutMs(kind);  // 1 試行の期限 (タイムアウト表)
    Request rq;
    rq.method = method;
    rq.path   = path;
    if (body != nullptr) {
        rq.content_type = "application/json";
        rq.body         = reinterpret_cast<const uint8_t*>(body);
        rq.body_len     = strlen(body);
    }
    Reply r = exchange(rq);
    // 証明書・時刻の失敗は送り直しても変わらないので送り直さない (TLS では再接続 = 再ハンドシェイク)。
    // hello は、1 回目の間に依頼が並んだら送り直さない (依頼を hello の 2 回目の後ろで待たせない)。
    const bool retry_ok = kind != link::RequestKind::Hello || link::helloRetryAllowed(commandsWaiting());
    if (retry_transport && retry_ok && r.status < 0 && r.status != kErrNoWifi && r.status != kErrAborted &&
        r.status != kErrCert && r.status != kErrClock) {
        // keep-alive の接続が edge 側で閉じられていた場合など。新しい接続で 1 回だけ送り直す。
        mclog::tagWarn(kTag, "{}: {} ({}); retry once", op, statusText(r.status), r.status);
        r = exchange(rq);
    }
    request_timeout_ms = config::EDGE_TIMEOUT_MS;  // 以後のリクエストは既定の期限
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
    cfg.host                  = url.host;  // edge_url で確かめた host (https はホスト名のみ)
    cfg.port                  = url.port;
    cfg.path                  = "/";
    if (url.https) {
        // 証明書は常に検証する (ESP-IDF の証明書バンドル。CN / SAN は host と照合。skip しない)。
        cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    } else {
        cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
    }
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
    if (http != nullptr) {
        esp_http_client_close(http);
    }
    http_open = false;
}

void EdgeWorker::dropHttp()
{
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
    if (url.https) {
        // https のソケットには暗号化された TLS レコード (TLS 1.3 のセッションチケットなど) が届いていることがあり、
        // 生のソケットを覗くと「読み残し」と見誤って毎回繋ぎ直す (= 毎回ハンドシェイク)。覗かずに使い、
        // 相手が閉じていたら次の open / write の失敗を request() の 1 回の再送で吸収する。
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
    Reply r;
    json_len = 0;
    json[0]  = '\0';
    if (rq.binary_out != nullptr) {
        rq.binary_out->clear();
    }
    // 途中で失敗したら、クライアントごと捨てる (読み残し・内部状態を次のリクエストに持ち越さない)。
    bool in_body_send = false;  // 本文の送信中 (期限切れでも「応答待ち」ではない)
    auto fail = [this, &r, &in_body_send](int status) {
        dropHttp();
        r.status = status;
        r.response_timeout = r.connected && status == kErrTimeout && !in_body_send;
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
    // ここから応答本文を読み終えるまでを 1 つの期限 (request_timeout_ms。hello は HELLO_TIMEOUT_MS) で縛る。
    const bool was_open = http_open;
    request_started_ms  = nowMs();
    armDeadline();  // 接続は設定されたタイムアウトを 1 回だけ使う
    // 前のやりとりの TLS / ソケットのエラーを消しておく (この open の失敗だけを分類する)。
    esp_http_client_get_and_clear_last_tls_error(http, nullptr, nullptr);
    esp_http_client_get_errno(http);
    const esp_err_t err = esp_http_client_open(http, static_cast<int>(rq.body_len));
    if (err != ESP_OK) {
        r.status = err == ESP_ERR_HTTP_CONNECT ? classifyOpenError() : kErrSend;
        dropHttp();
        return r;
    }
    http_open   = true;
    r.connected = true;
    if (was_open) {
        ++stats_conn_reused;
    } else {
        ++stats_conn_new;
    }  // ここから先の期限切れは「接続後の応答待ち」(本文の送信中を除く)
    if (!was_open) {
        // 本文の最後の端数セグメントが Nagle で遅れないようにする。
        const int fd  = esp_http_client_get_socket(http);
        const int one = 1;
        if (fd >= 0) {
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        }
    }

    // 本文。大きいフレームは区切って書き、合間に期限と終了要求を見る。
    size_t sent  = 0;
    in_body_send = true;
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
    in_body_send = false;

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

int EdgeWorker::classifyOpenError()
{
    // esp_http_client_open() の接続の失敗を、esp-tls が残したエラーで分ける (link::classifyConnectFailure)。
    link::ConnectError e;
    e.https          = url.https;
    int tls_code     = 0;
    int cert_flags   = 0;
    e.tls_last_error = static_cast<int>(esp_http_client_get_and_clear_last_tls_error(http, &tls_code, &cert_flags));
    e.sock_errno     = esp_http_client_get_errno(http);
    e.cert_flags     = cert_flags;
    e.clock_valid    = clockValidNow();
    const link::ConnectFailure f = link::classifyConnectFailure(e);
    mclog::tagWarn(kTag, "connect failed: kind {} (esp-tls 0x{:x}, tls code 0x{:x}, cert flags 0x{:x}, errno {})",
                   static_cast<unsigned>(f), static_cast<unsigned>(e.tls_last_error), static_cast<unsigned>(tls_code),
                   static_cast<unsigned>(cert_flags), e.sock_errno);
    switch (f) {
        case link::ConnectFailure::Dns:
            return kErrDns;
        case link::ConnectFailure::Cert:
            return kErrCert;
        case link::ConnectFailure::ClockNotSynced:
            return kErrClock;
        case link::ConnectFailure::Tls:
            return kErrTls;
        case link::ConnectFailure::Tcp:
            break;
    }
    return kErrConnect;
}

bool EdgeWorker::armDeadline()
{
    // リクエスト全体 (接続 + 送信 + ヘッダ + 本文) の期限までの残りを、次のブロッキング呼び出しの
    // タイムアウトにする。期限を過ぎていれば false (呼び出し側はタイムアウトとして打ち切る)。
    //
    // 既知の制限: esp_http_client_open() と esp_http_client_fetch_headers() は内部でループし、
    // このタイムアウトを送受信 1 回ごとに適用する。応答を少しずつ返す相手では、リクエスト全体が
    // 期限を超え得る。LAN 内の edge が相手なので許容する。アプリを閉じるときは end() が 1 秒で
    // net タスクを切り離すので、実害は「切り離されたタスクが残っている間 begin() を断る」までに限られる。
    // (ソケットを外から強制的に切る方式は、切る側のタスクのブロックと fd の取り違えのリスクが
    //  上回るため採らない。docs/design/fw-app-step2.md §4.5)
    const uint32_t elapsed = nowMs() - request_started_ms;
    if (elapsed >= request_timeout_ms) {
        return false;
    }
    const int remaining = static_cast<int>(request_timeout_ms - elapsed);
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
    {
        // DIAG の「再接続」で置いた「再接続中」は成功で消す。それ以外は「最後の通信エラー」として残す
        // (edge_client.h の lastError())。
        std::lock_guard<std::mutex> lock(mutex);
        if (link::clearErrorOnSuccess(last_error)) {
            last_error[0] = '\0';
        }
    }
    last_ok_ms.store(nowMs());
    ever_ok.store(true);
    starting.store(false);
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
    const bool frames_active = !frame_stats.empty();
    if (frames_active || stats_failures > 0) {
        const frame::Summary f = frame_stats.summarize(dt);
        mclog::tagInfo(kTag,
                       "send {} frames ({}) in {} ms ({:.1f} fps), attempts {}, frame failures {} (send {}, encode {}), "
                       "rtt avg {} max {} ms, edge avg {} ms, failures {}, skipped {}, stack free {}",
                       f.sent, frame::formatHeader(kFrameFormat), dt, f.fps, f.attempts,
                       f.send_failures + f.encode_failures, f.send_failures, f.encode_failures, f.rtt_avg_ms,
                       f.rtt_max_ms, f.edge_avg_ms, stats_failures, f.skipped, uxTaskGetStackHighWaterMark(nullptr));
        if (kFrameFormat == frame::Format::Jpeg && (f.encoded > 0 || f.encode_failures > 0)) {
            mclog::tagInfo(kTag, "jpeg: encoded {}, jpeg_encode_ms avg {} max {}, size avg {} max {} B, failed {}",
                           f.encoded, f.encode_avg_ms, f.encode_max_ms, f.jpeg_avg_bytes, f.jpeg_max_bytes,
                           f.encode_failures);
        }
    }
    if (stats_hello > 0) {
        mclog::tagInfo(kTag, "hello {} (ok {}, starting {}) rtt avg {} max {} ms, link {}, stack free {}", stats_hello,
                       stats_hello_ok, stats_hello_starting, stats_hello_rtt_sum / stats_hello, stats_hello_rtt_max,
                       online() ? "online" : (starting.load() ? "starting" : "offline"),
                       uxTaskGetStackHighWaterMark(nullptr));
    }
    if (stats_conn_new > 0 || stats_conn_reused > 0) {
        mclog::tagInfo(kTag, "connections: new {} reused {} ({})", stats_conn_new, stats_conn_reused,
                       url.https ? "https" : "http");
    }
    if (frames_active || stats_hello > 0) {
        // 内部 RAM の最小空き (起動からの最小) と今の最大連続領域。TLS と esp_new_jpeg のワーク領域が使う (§3.4)
        mclog::tagInfo(kTag, "heap: internal free {} min {} largest {}, psram free {} largest {}",
                       heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    stats_conn_new    = 0;
    stats_conn_reused = 0;
    stats_since_ms = now;
    stats_hello          = 0;
    stats_hello_ok       = 0;
    stats_hello_starting = 0;
    stats_hello_rtt_sum  = 0;
    stats_hello_rtt_max  = 0;
    stats_failures = 0;
    frame_stats.reset();
}

}  // namespace photobooth::net

#endif  // PHOTOBOOTH_EDGE_ENABLED
