# ステップ2b 設計: device を Wi-Fi で edge に繋ぎ、顔判定と首振りを実機で動かす

対象: `docs/spec.md` §11 の実装順 2 の残り（device 側）と、それに必要な edge の小さな追加。
前提: ステップ1（PR #1, #2）とステップ2a（PR #3）がマージ済み。通信契約は `docs/protocol.md`。

## 1. ゴールと非ゴール

ゴール:

- K151 が Wi-Fi に繋がり、`hello` で edge の接続状態を IDLE 画面に出す（「PC接続中」/「PC未接続」）。
- COMPOSE / CAPTURE 中に QVGA RGB565 フレームを edge に送り、`frame_result` の `servo_dx/dy` で首を寄せ、`face_count/target` と `hint` を画面に出す。
- `accepted` で撮影を終え、`review save` → `photo` で受け取った本物の URL と削除時刻を PHOTO_QR / X_QR に出す。
- 時間切れなら edge の候補（JPEG）を REVIEW に表示し、「保存する」/「撮り直す」を edge に伝える。
- edge に繋がらないときは自動判定つきの撮影を始めず、タッチで診断画面を開ける（spec §9）。

非ゴール:

- 音声起動（ステップ3）。本物の gallery（ステップ4）。閾値・ゲインの本調整（ステップ5。ただし config で変えられる状態にする）。

## 2. 技術選定（追加分）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| Wi-Fi / HTTP | Arduino core 同梱の `WiFi.h`、`HTTPClient.h`（`setReuse(true)` で keep-alive） | 追加ライブラリなし。protocol.md は 1 フレーム 1 リクエストの同期 HTTP |
| JSON | Arduino core 同梱ではないので、`bblanchon/ArduinoJson@7.4.3` を lib_deps に追加 | frame_result / photo の JSON を安全に読む。手書きパーサは避ける |
| 候補画像の表示 | M5GFX の `drawJpg()`（`edge` が返す JPEG を描画） | device 側の保持フレームと edge の候補がずれないようにする |
| 通信の実行場所 | FreeRTOS タスク（core 0、スタック 8 KB）と単一スロットの mailbox | app のループ（プレビュー・タッチ）を HTTP の往復で止めない。古い未処理フレームを捨てる（spec §6.1）|

`platformio.ini` の追加は ArduinoJson の 1 行だけ。その他の版は変えない。

## 3. edge 側の追加（小）

- `GET /v1/sessions/{session_id}/candidate` → `image/jpeg`（採用フレームがあればそれ、無ければ最良候補。どちらも無ければ 404 `no_candidate`）。JPEG は品質 80、サイズは受信フレームのまま（QVGA）。`Cache-Control: no-store`。
- `protocol.md` にこのエンドポイントを追加する。テスト: accepted 後と timeout 後に JPEG が返り、候補なしで 404、cancel 後は 404。

## 4. device 側の構成（追加・変更）

```
device/src/
├── net/
│   ├── wifi_link.h/.cpp      # Wi-Fi 接続・再接続。状態 (disconnected/connecting/connected)、IP、RSSI
│   └── http_edge_client.h/.cpp  # EdgeClient の実装。FreeRTOS タスク + mailbox
├── edge/edge_client.h        # 既存。FrameResult に hint と latency_ms を追加、pollCandidate() を追加
├── app/app.cpp               # COMPOSE/CAPTURE/REVIEW/UPLOADING/DIAG の分岐を edge の結果で埋める
├── ui/screens.cpp            # 人数/目標、ヒント帯、診断画面、候補 JPEG の表示、削除時刻
└── data/closer.wav           # 「もう少し寄ってね」(tools/make_voice.sh に追加)
```

### 4.1 EdgeClient インターフェースの変更

```cpp
struct FrameResult {
  bool valid; uint32_t frame_id; bool dropped;
  uint8_t face_count, target_face_count;
  bool all_in_frame, all_eyes_open, all_smiling;
  int servo_dx, servo_dy;
  Hint hint;            // None / Closer / TooMany
  bool accepted;
  uint16_t latency_ms;
};
class EdgeClient {
  virtual bool isOnline();                       // 直近 5 秒以内に hello か任意の応答が成功
  virtual void sessionStart(const Session&);      // 非同期 (コマンドキュー)
  virtual bool offerFrame(const Session&, const camera_fb_t&, int servo_x, int servo_y, Phase);
                                                  // スロットへコピー。送信中なら上書き (newest wins)。戻り値はコピーできたか
  virtual bool pollResult(FrameResult&);          // 最新の結果を 1 回だけ返す
  virtual void sessionTimeout(const Session&);    // 非同期。結果は pollTimeout()
  virtual bool pollTimeout(bool& has_candidate);
  virtual bool fetchCandidate(uint8_t*& jpeg, size_t& len);   // 同期 (最大 3 秒)。PSRAM に確保、呼び出し側が free
  virtual void reviewDecision(const Session&, bool save);     // save は最大 3 回再試行 (protocol.md)
  virtual bool pollPhotoReady(PhotoInfo&);        // ready / error / pending
  virtual void sessionCancel(const Session&);
  virtual const char* lastError();                // 診断画面用
};
```

`sendFrame` は `offerFrame` に改名する（コピーして即 return、という契約を名前で表す）。`NullEdge` は残す（`PHOTOBOOTH_NO_EDGE` ビルドフラグで選べるようにし、ステップ1の挙動を再現できるようにする）。

### 4.2 HttpEdgeClient の構造

- **タスク**: `net_task` が 1 本。ループで (1) コマンドキュー（start / timeout / review / cancel / photo ポーリング）を処理、(2) フレームスロットにデータがあれば POST、(3) オフラインなら `HELLO_INTERVAL_MS`（5 秒）ごとに hello。
- **フレームスロット**: PSRAM の 153,600 バイト × 1。`offerFrame` は mutex を取って memcpy し `has_frame=true`。net_task は mutex 下でローカルへ「所有権を移す」（ポインタの swap で 2 面にして memcpy を避ける）。送信中に新しいフレームが来たらスロットが上書きされ、古いものは送られない。
- **結果 mailbox**: `FrameResult` 1 つ + `fresh` フラグ。`pollResult` は fresh なら返して false にする。
- **HTTP**: `HTTPClient` を 1 つ、`setReuse(true)`、`setTimeout(EDGE_TIMEOUT_MS=3000)`。ヘッダは protocol.md どおり（`X-Device-Id`, `X-Device-Key`, `X-Frame-Id`, `X-Capture-Ms`, `X-Servo-X/Y`, `X-Width/Height`, `X-Format: rgb565`, `X-Phase`, `Content-Type: application/octet-stream`, `Content-Length` は HTTPClient が付ける）。
- **online 判定**: 直近 5 秒以内に 2xx を受けたら online。3 回連続で失敗したら offline にして hello から再開。
- **ログ**: 送信 fps、往復 latency（frame_result の `latency_ms` と device 側の実測）、失敗理由。鍵・URL・画像は出さない。

### 4.3 状態機械の変更（app.cpp）

| 状態 | 変更 |
| --- | --- |
| IDLE | 画面右下に「PC接続中」/「PC未接続」。online ならタッチで `sessionStart` → ANNOUNCE。offline ならタッチで DIAG |
| DIAG（新設） | SSID、IP、RSSI、edge host:port、最後のエラー、「再接続」「判定なしで撮影」「戻る」。「判定なしで撮影」は NullEdge 相当の固定フローで、UPLOADING で「PC未接続のため保存できません」の ERROR にする |
| COMPOSE | 首が止まっているときだけ `offerFrame(phase=compose)`。結果の `servo_dx/dy` を `head_.nudge()` に渡す。`face_count>=1 && all_in_frame` が 1 秒連続、または 5 秒で CAPTURE。`hint=Closer` は帯に「もう少し寄ってね」+ `closer.wav`（1 回だけ）|
| CAPTURE | 同様に送る（首が動いている間は送らない）。右上に `人数 face/target` と残り秒。`accepted` が来たら「撮れたよ」を出して UPLOADING。10 秒経過後に届いた結果は捨てる（`state_since` 比較）。時間切れで `sessionTimeout` → `pollTimeout` |
| REVIEW | `has_candidate` なら `fetchCandidate` で JPEG を取り `drawJpg`。取れなければ device 保持のフレーム（ステップ1の候補）を出し、ログに warn。「保存する」→ `reviewDecision(save)` → UPLOADING、「撮り直す」→ `reviewDecision(retake)` → 新セッションで ANNOUNCE |
| UPLOADING | `accepted` 経由でも `reviewDecision(save)` を送る（protocol.md）。`pollPhotoReady` が ready で PHOTO_QR、error で ERROR（「写真を保存できませんでした」+ 再試行=save 再送、最大 3 回）。上限 15 秒 |
| PHOTO_QR / X_QR | `photo_url` / `share_url` を QR に。削除時刻は `expires_at`（ISO 8601, +09:00）の `HH:MM` |
| ERROR | 通信失敗の理由（`lastError()`）を出す。終了で `sessionCancel` |

- 首の指示は device が最終判断する: `nudge()` の 3°/500 ms/可動域のクランプはそのまま。edge の値は「希望」。
- 撮影中の再タッチは無視（変更なし）。
- 起動時: Wi-Fi 接続を最大 10 秒待ってから IDLE へ（待ち中は「Wi-Fi接続中」）。繋がらなくても IDLE には入る。

### 4.4 UI の追加

- IDLE 右下の接続表示（既存の「PC未接続」を状態で切り替え）。
- COMPOSE / CAPTURE の帯: 「みんな画面に入ってね」→ hint に応じて「もう少し寄ってね」「4人までだよ」。
- CAPTURE 右上: `人数 2/2` の形式（target が 0 のときは `--`）。
- DIAG 画面: 本文に 5 行の診断、ボタン 2 つ + 「戻る」は画面上部のタップで代用しない（ボタン帯は最大 2 つなので「再接続」「判定なしで撮影」とし、「戻る」は頭部タッチに割り当てる）。
- REVIEW: JPEG を全面に描き、既存のタイトル帯とボタン帯を重ねる。

## 5. config の追加（`config.example.h`）

```cpp
constexpr uint32_t EDGE_TIMEOUT_MS   = 3000;
constexpr uint32_t HELLO_INTERVAL_MS = 5000;
constexpr uint32_t WIFI_BOOT_WAIT_MS = 10000;
constexpr uint32_t COMPOSE_STABLE_MS = 1000;   // 既存
constexpr uint32_t UPLOAD_WAIT_MS    = 15000;
constexpr uint8_t  UPLOAD_RETRY      = 3;
```

`HEAD_GAIN_X / HEAD_GAIN_Y` は削除する（px → 角度の変換は edge の `[head] gain_x/y` に一本化し、device は受け取った `servo_dx/dy` をそのまま `nudge()` に渡す。§7 参照）。

`EDGE_HOST` は PC の LAN アドレス。README に「`ipconfig getifaddr en0` で調べる」「edge は `--host 0.0.0.0` か PC のアドレスで起動する」「鍵を device と edge で揃える」を書く。

## 6. 受け入れチェック（PR テンプレートに転記）

- [ ] 起動後 IDLE に「PC接続中」が出る。edge を止めると 5〜10 秒で「PC未接続」になり、再起動すると戻る。
- [ ] 「PC未接続」でタッチすると診断画面が出て、SSID / IP / edge host / エラーが読める。
- [ ] COMPOSE でカメラの前に顔を出すと 1 秒で CAPTURE に進む。顔を左右にずらすと首が同じ方向に寄る（ずれるなら `HEAD_GAIN_X` の符号を反転）。上下も同様。
- [ ] CAPTURE 中に人数と目標が出る。1 人で目を開けて笑うと「撮れたよ」→ QR。目を閉じる／笑わないと採用されない。
- [ ] 2 人で、1 人が笑わないと採用されない。
- [ ] 時間切れで edge の候補が REVIEW に出る。保存で QR、撮り直しで新しい 10 秒。
- [ ] 写真 QR をスマホで読むとモックの写真ページが開く（同じ Wi-Fi）。削除時刻が画面と一致する。
- [ ] 送信 fps が 2 以上（ログ）。プレビューが止まらない。
- [ ] 「もう少し寄ってね」が出る条件（顔を画面端に寄せる）で音声と帯が出る。
- [ ] edge のログに画像・トークン・URL が出ない。device のログに鍵が出ない。

## 7. 実機で校正して config に反映する値

- `HEAD_GAIN_X / Y` の符号と大きさ（edge 側は `[head] gain_x/y`。**どちらか一方だけ**で校正し、もう一方は 1.0 の素通しにする。決定: edge が px→1/10 度の変換を持ち、device は `HEAD_GAIN_*` を削除して受け取った値をそのまま `nudge()` に渡す）。
- `rgb565_byte_order`（edge 設定）。色が反転して見えたら `big` に。
- Wi-Fi 経由の実 fps と latency。2 fps を切るなら JPEG 送信（device で `frame2jpg`）を検討する（ステップ5）。
