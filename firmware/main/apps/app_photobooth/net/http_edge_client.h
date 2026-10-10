/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// EdgeClient の HTTP 実装 (docs/design/fw-app-step2.md §3, docs/protocol.md)。
// 独立ファーム版 device/src/net/http_edge_client の移植 (HTTPClient → esp_http_client)。
//
// 通信は net タスク 1 本だけが行う。Flow (onRunning のスレッド) との受け渡しは次の 3 つ:
//   - コマンドキュー: start / timeout / review / cancel / candidate / reconnect
//   - フレームスロット: PSRAM の 2 面。Flow が書く面 (write) と net タスクが送る面 (send) を
//     mutex 下のインデックス交換で入れ替える (所有権の移動。送信中の面に Flow は触らない)。
//     送信中に次のフレームが来たら write 面を上書きする (newest wins、キューを溜めない)。
//   - mailbox: frame_result / timeout / photo / candidate の最新 1 件 + fresh フラグ (mutex)。
// セッションの世代 (generation) を sessionStart / sessionCancel で進め、古い世代の結果は捨てる。
//
// タスクの寿命は hw/camera・shared/hw/audio と同じ worker + shared_ptr 方式: end() はタスクを 1 秒待ち、
// 止まらなければ共有状態 (EdgeWorker) ごと切り離す。後始末はタスクが終わるときに自分でする。
// 切り離したタスクが生きている間は begin() を断る (診断画面の「再接続」でやり直せる)。
//
// net タスクから LVGL・画面の HAL は呼ばない。鍵・URL・トークン・画像バイトはログに出さない。
#pragma once

#include <memory>

#include "../config.h"
#include "edge_client.h"

namespace photobooth::net {

struct EdgeWorker;

// ビルド設定に合った EdgeClient を作る (edge 有効なら HttpEdgeClient、無効なら NullEdge)。
std::unique_ptr<EdgeClient> createEdgeClient();

#if PHOTOBOOTH_EDGE_ENABLED

class HttpEdgeClient : public EdgeClient {
public:
    HttpEdgeClient() = default;
    ~HttpEdgeClient() override;
    HttpEdgeClient(const HttpEdgeClient&)            = delete;
    HttpEdgeClient& operator=(const HttpEdgeClient&) = delete;

    // フレームスロット (PSRAM) を確保して net タスクを起動する (Wi-Fi の接続は app_photobooth.cpp が先に済ませる)。
    // 失敗したら false (以後 isOnline() は false のまま。理由は lastError())。
    bool begin() override;
    void end() override;

    bool isOnline() override;
    LinkState linkState() override;
    void setLatencySensitive(bool active) override;
    void sessionStart(const Session& s) override;
    bool offerFrame(const Session& s, const hw::FrameView& frame, int servo_x, int servo_y, Phase phase) override;
    bool pollResult(FrameResult& out) override;
    void sessionTimeout(const Session& s) override;
    bool pollTimeout(bool& ok, bool& has_candidate) override;
    void requestCandidate(const Session& s, uint32_t timeout_ms, bool allow_retry) override;
    bool pollCandidate(bool& ok, JpegBytes& jpeg) override;
    void reviewDecision(const Session& s, bool save) override;
    bool pollPhotoReady(PhotoInfo& out) override;
    void sessionCancel(const Session& s) override;
    const char* lastError() override;
    void diagnostics(Diagnostics& out) override;
    void reconnect() override;

    // 切り離した net タスクがまだ終わっていないか (アプリの開き直しをまたいで覚えている)。
    static bool busy();

private:
    // タスクと共有する状態。タスクも shared_ptr を持つので、こちらが先に消えても安全。
    std::shared_ptr<EdgeWorker> worker_;

    // ---- Flow のスレッドだけが触る ----
    bool cand_waiting_          = false;  // requestCandidate() の結果を待っている
    uint32_t cand_timeout_ms_   = 0;      // 今の依頼の期限 (pollCandidate の待ち時間の上限に使う)
    uint32_t cand_seq_          = 0;      // 候補 JPEG の依頼番号
    uint32_t cand_requested_ms_ = 0;
    char begin_error_[96]       = {};     // worker_ が無いときの lastError()
    char error_copy_[96]        = {};
};

#endif  // PHOTOBOOTH_EDGE_ENABLED

}  // namespace photobooth::net
