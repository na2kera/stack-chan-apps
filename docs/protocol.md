# Device–Edge 通信契約 (protocol_version 1)

`docs/spec.md` §8 のイベントを HTTP/1.1 に載せる。device が client、edge が server。edge は LAN 内でだけ listen する。

## 決定事項

- **トランスポート**: HTTP/1.1 + JSON、フレームは `application/octet-stream`。device は keep-alive で 1 本の接続を使い回す。WebSocket は使わない（device 側の追加ライブラリを避け、フレーム→判定結果を同期の request/response にすることでバッファ所有権を単純にする）。
- **1 フレーム 1 リクエスト**: device は前のフレームの応答を受けてから次を送る。edge は同じセッションのフレームを到着順に 1 枚ずつ処理する。`frame_id` が処理済みより古いものは処理せず `dropped: true` を返す。
- **認証**: 全リクエストに `X-Device-Id` と `X-Device-Key`（共有鍵）。不一致は 401。鍵は device の `config.h` と edge の `config.toml` / 環境変数にだけ置く。
- **時計**: `capture_ms` は device の単調時計（セッション開始からの ms）。edge の壁時計は写真の有効期限にだけ使う。
- **フレーム形式**: `X-Format: rgb565`（QVGA、esp_camera の出力バイト列そのまま）を初期値とする。edge は `jpeg` も受け付ける。RGB565 のバイト順は edge 設定 `rgb565_byte_order`（`little` / `big`）で切り替え、実機で確認する。

## エンドポイント

ベース URL: `http://<edge-host>:8765`

| spec §8 イベント | メソッドとパス | 本文 / ヘッダ | 応答 |
| --- | --- | --- | --- |
| `hello` | `POST /v1/hello` | `{ "device_id", "protocol_version": 1 }` | `200 { "ready": true, "edge_state": "idle", "max_faces": 4, "countdown_sec": 10 }` |
| `session_start` | `POST /v1/sessions` | `{ "session_id", "started_at_ms" }` | `200 { "ok": true }`。同じ `session_id` の再送は状態を初期化する |
| `frame` | `POST /v1/sessions/{session_id}/frames` | ヘッダ `X-Frame-Id`, `X-Capture-Ms`, `X-Servo-X`, `X-Servo-Y`, `X-Width`, `X-Height`, `X-Format`, `X-Phase` (`compose` / `capture`)。本文は画像バイト列 | `200` frame_result（下記） |
| `session_timeout` | `POST /v1/sessions/{session_id}/timeout` | なし | `200 { "candidate": { "frame_id", "score", "reason" } }` または `{ "candidate": null }` |
| `review_decision` | `POST /v1/sessions/{session_id}/review` | `{ "decision": "save" \| "retake" }` | save: `202 { "status": "uploading" }`。retake: `200 { "ok": true }`（保持フレームを破棄） |
| `photo_ready` | `GET /v1/sessions/{session_id}/photo` | なし | `200 { "status": "pending" }` / `{ "status": "ready", "photo_url", "share_url", "expires_at" }` / `{ "status": "error", "reason" }` |
| `session_cancel` | `POST /v1/sessions/{session_id}/cancel` | なし | `200 { "ok": true }`（未公開の候補を破棄） |
| `audio_clip` | `POST /v1/audio` | ステップ3で決める（VAD で区切った PCM クリップの POST） | `{ "start_requested": bool }` |

未知の `session_id` は 404。`session_id` は device が生成する UUID v4。

### frame_result

```json
{
  "session_id": "…",
  "frame_id": 42,
  "dropped": false,
  "face_count": 2,
  "target_face_count": 2,
  "all_in_frame": true,
  "all_eyes_open": true,
  "all_smiling": false,
  "servo_dx": 0,
  "servo_dy": -20,
  "hint": null,
  "accepted": false,
  "latency_ms": 38
}
```

- `servo_dx` / `servo_dy`: 首を動かす相対量（1/10 度）。device 側でも可動域と 1 回の上限にクランプする。0 は動かさない。
- `hint`: `"closer"`（首を振っても全員が入らない）/ `"too_many"`（上限超え）/ `null`。device は画面と音声で案内する。
- `accepted`: true なら edge はこのフレームのバイト列を保持済み。device は UPLOADING へ進む。10 秒経過後に返った `accepted` は device 側で無視する。

## タイムアウトと再試行

- device は各リクエストに 3 秒のタイムアウトを置く。失敗しても同じフレームを再送しない（次のフレームを送る）。
- `review` の save だけは同一 `session_id` で最大 3 回まで再試行できる（edge 側は冪等）。
- edge は 5 分以上イベントの無いセッションを破棄する。
- edge に繋がらない間、device は「PC未接続」を出し、自動判定つきの撮影を始めない（タッチによる撮影は固定カメラで続けられる）。
