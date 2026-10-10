/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// edge との通信の抽象インターフェース (docs/design/fw-app-step2.md §3)。
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
#include "edge_types.h"

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

// frame の X-Phase。
enum class Phase : uint8_t { Compose, Capture };

// 待機画面に出す接続状態 LinkState は edge_types.h (ロジック層と共有)。

// 診断画面 (DIAG) に出す接続状態。鍵は含めない。
struct Diagnostics {
    enum class Wifi : uint8_t { NotConfigured, Disconnected, ConfigMode, Connected };
    bool enabled = false;  // edge 通信ありのビルドか (NullEdge は false)
    Wifi wifi    = Wifi::Disconnected;
    bool online  = false;  // edge から直近に 2xx を受けている
    char ssid[33]       = {};
    char ip[16]         = {};
    int rssi            = 0;  // dBm。wifi == Connected のときだけ有効
    char edge_url[96]   = {};  // "scheme://host:port" (path と鍵は含めない)。書式が不正なら ""
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

    // hello が後続の依頼 (timeout / candidate / save / photo) を妨げる段階か (Flow が毎周期知らせる)。
    // 判定つきの撮影の開始から写真の準備完了 (QR を出す) までが true。その間は定期 hello を送らず、
    // 接続状態は frame などの結果で決める。QR 表示中は false (hello で warm を保ち、停止にも気づく)。
    virtual void setLatencySensitive(bool active) = 0;

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
    // timeout_ms はリクエスト全体の期限、allow_retry は通信失敗のとき 1 回だけ送り直すか。
    // 既定 (0, true) は REVIEW 用 (EDGE_TIMEOUT_MS)。SHUTTER は短い期限で 1 回だけ試す。
    virtual void requestCandidate(const Session&, uint32_t timeout_ms = 0, bool allow_retry = true) = 0;
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
