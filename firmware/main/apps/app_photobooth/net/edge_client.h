/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge (PC) との通信の抽象インターフェース (docs/design/fw-app-step2.md §3)。
// 独立ファーム版 device/src/edge/edge_client.h の最終形の移植。イベント名は docs/spec.md §8、
// HTTP の契約は docs/protocol.md と一致させる。
//
// 実装 (net::HttpEdgeClient) は通信を net タスクで行うので、sessionStart などは「依頼して即 return」、
// 結果は poll*() で受け取る。Flow は onRunning のスレッドからだけ呼ぶ (どのメソッドもブロックしない)。
//
// 独立ファーム版との違い:
//   - begin() / end() を持つ (アプリを開いている間だけタスクと PSRAM を使う)。
//   - 候補 JPEG は同期の fetchCandidate() ではなく requestCandidate() / pollCandidate() の非同期
//     (Mooncake のループを最大 3 秒止めないため)。
//   - 待機画面の接続表示のために linkState() を持つ。
#pragma once

#include <esp_heap_caps.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

#include "../flow/session.h"
#include "../hw/camera.h"

namespace photobooth::net {

// PSRAM に確保するアロケータ (候補 JPEG 用)。確保できなければ std::bad_alloc。
template <class T>
struct PsramAllocator {
    using value_type = T;
    PsramAllocator() = default;
    template <class U>
    PsramAllocator(const PsramAllocator<U>&) noexcept
    {
    }
    T* allocate(size_t n)
    {
        void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (p == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) noexcept
    {
        heap_caps_free(p);
    }
    template <class U>
    bool operator==(const PsramAllocator<U>&) const noexcept
    {
        return true;
    }
    template <class U>
    bool operator!=(const PsramAllocator<U>&) const noexcept
    {
        return false;
    }
};

// 候補 JPEG のバイト列 (PSRAM)。
using JpegBytes = std::vector<uint8_t, PsramAllocator<uint8_t>>;

// frame_result の hint (protocol.md)。
enum class Hint : uint8_t { None, Closer, TooMany };

// frame の X-Phase。
enum class Phase : uint8_t { Compose, Capture };

// 待機画面に出す接続状態。
enum class LinkState : uint8_t {
    Offline,  // 「PC未接続」(edge 無効ビルドもこれ)
    Online,   // 「PC接続中」
};

// frame_result (spec §8, protocol.md)。
struct FrameResult {
    bool valid                = false;
    uint32_t frame_id         = 0;
    bool dropped              = false;
    uint8_t face_count        = 0;
    uint8_t target_face_count = 0;
    bool all_in_frame         = false;
    bool all_eyes_open        = false;
    bool all_smiling          = false;
    int servo_dx              = 0;  // 1/10 度。device は Head::nudge() でさらにクランプする
    int servo_dy              = 0;
    Hint hint                 = Hint::None;
    bool accepted             = false;
    uint16_t latency_ms       = 0;  // edge 側の処理時間
};

// PhotoInfo::reason: save の送信回数 (UPLOAD_RETRY) を使い切ったので送らなかった。
inline constexpr const char* kSaveRetryExhausted = "retry_exhausted";

// photo_ready (GET …/photo)。
struct PhotoInfo {
    enum class Status : uint8_t { Pending, Ready, Error };
    Status status       = Status::Pending;
    char photo_url[256] = {};
    char share_url[256] = {};
    char expires_at[32] = {};  // ISO 8601 (+09:00)
    char reason[32]     = {};  // status == Error のとき (例 "upload_failed")
};

// 診断画面 (DIAG) に出す接続状態。鍵は含めない。
struct Diagnostics {
    enum class Wifi : uint8_t { NotConfigured, Disconnected, ConfigMode, Connected };
    bool enabled = false;  // edge 通信ありのビルドか (NullEdge は false)
    Wifi wifi    = Wifi::Disconnected;
    bool online  = false;  // edge から直近に 2xx を受けている
    char ssid[33]       = {};
    char ip[16]         = {};
    int rssi            = 0;  // dBm。wifi == Connected のときだけ有効
    char edge_host[64]  = {};
    uint16_t edge_port  = 0;
    char last_error[96] = {};
};

class EdgeClient {
public:
    virtual ~EdgeClient() = default;

    // 通信を始める (net タスクの起動)。Wi-Fi の接続は含まない。失敗しても撮影は判定なしで続けられる。
    virtual bool begin() = 0;
    // 通信を止める。Wi-Fi は切断しない (純正の他のアプリが使う)。
    virtual void end() = 0;

    // 直近に hello か任意のリクエストで 2xx を受けていれば true。
    virtual bool isOnline()       = 0;
    virtual LinkState linkState() = 0;

    // 非同期 (コマンドキュー)。以前のセッションの結果・保留フレームは捨てる。
    virtual void sessionStart(const Session&) = 0;

    // フレーム (RGB565 LE) をスロットへコピーして即 return する (frame は直後に呼び出し側が unlock する)。
    // 送信中なら次に送るフレームを上書きする (newest wins)。frame_id は s.frame_id。戻り値はコピーできたか。
    virtual bool offerFrame(const Session& s, const hw::FrameView& frame, int servo_x, int servo_y, Phase phase) = 0;

    // 最新の frame_result を 1 回だけ返す。新しい結果が無ければ false。
    virtual bool pollResult(FrameResult& out) = 0;

    // 非同期。結果は pollTimeout()。
    virtual void sessionTimeout(const Session&) = 0;
    // session_timeout の結果が出たら true (1 回だけ)。ok=true は 200 で JSON を読めたときだけで、
    // そのとき has_candidate が候補の有無。ok=false は通信失敗・200 以外・JSON 不正
    // (edge 再起動後の 404 など。理由は lastError())。「候補なし」とは区別する。
    virtual bool pollTimeout(bool& ok, bool& has_candidate) = 0;

    // 候補 JPEG (GET …/candidate) を取りに行く (非同期)。結果は pollCandidate()。
    virtual void requestCandidate(const Session&) = 0;
    // 結果が出たら true (1 回だけ)。ok=true なら jpeg に JPEG が入る。取れなかった、または
    // EDGE_TIMEOUT_MS + 余裕 の間に結果が出なかったら ok=false。
    virtual bool pollCandidate(bool& ok, JpegBytes& jpeg) = 0;

    // 非同期。save は通信に失敗したら送り直すが、同じセッションへの save の送信は (この呼び出しを
    // 何度しても) 合計 UPLOAD_RETRY 回まで。使い切ったら送らずに pollPhotoReady() が
    // Error(kSaveRetryExhausted) を返す。成功したら photo をポーリングする。
    virtual void reviewDecision(const Session&, bool save) = 0;
    // 写真の準備が終わった (ready / error) ら true (1 回だけ)。pending の間は false。
    virtual bool pollPhotoReady(PhotoInfo& out) = 0;

    virtual void sessionCancel(const Session&) = 0;

    // 診断画面用。最後の通信エラーの短い説明 (鍵・URL は含めない)。無ければ ""。
    virtual const char* lastError()           = 0;
    virtual void diagnostics(Diagnostics& out) = 0;
    // edge との接続を捨てて、すぐ hello する (診断画面の「再接続」)。Wi-Fi には触らない。
    virtual void reconnect() = 0;
};

}  // namespace photobooth::net
