# Device–Edge 通信契約 (protocol_version 1)

`docs/spec.md` §8 のイベントを HTTP/1.1 に載せる。device が client、edge が server。edge は LAN 内でだけ listen する。

クラウド構成（`edge-cloud/`、[docs/design/step5-edge-cloud.md](design/step5-edge-cloud.md)）では、ベース URL が `https://<worker-host>` になり、前段の Worker が同じ `X-Device-Id` / `X-Device-Key` を確かめてから edge に渡す。エンドポイントと応答は変わらない。

## 決定事項

- **トランスポート**: HTTP/1.1 + JSON、フレームは `application/octet-stream`。device は keep-alive で 1 本の接続を使い回す。WebSocket は使わない（device 側の追加ライブラリを避け、フレーム→判定結果を同期の request/response にすることでバッファ所有権を単純にする）。
- **1 フレーム 1 リクエスト**: device は前のフレームの応答を受けてから次を送る。edge は同じセッションのフレームを到着順に 1 枚ずつ処理する。`frame_id` が処理済みより古いものは処理せず `dropped: true` を返す。
- **認証**: 全リクエストに `X-Device-Id` と `X-Device-Key`（共有鍵）。不一致は 401。鍵は device の `config.h` と edge の `config.toml` / 環境変数にだけ置く。
- **時計**: `capture_ms` は device の単調時計（セッション開始からの ms）。edge の壁時計は写真の有効期限にだけ使う。
- **フレーム形式**: `X-Format: rgb565`（QVGA、esp_camera の出力バイト列そのまま）を初期値とする。edge は `jpeg` も受け付ける。K151 のファームは 6c から既定で `jpeg`（品質 80、`X-Width` / `X-Height` は元の 320 / 240。`docs/design/step6-cloud-device.md` §3.3）を送り、`-DPHOTOBOOTH_FRAME_RGB565=1` でビルドすると `rgb565`。RGB565 のバイト順は edge 設定 `rgb565_byte_order`（`little` / `big`）で切り替え、実機で確認する。

## エンドポイント

ベース URL: `http://<edge-host>:8765`

| spec §8 イベント | メソッドとパス | 本文 / ヘッダ | 応答 |
| --- | --- | --- | --- |
| `hello` | `POST /v1/hello` | `{ "device_id", "protocol_version": 1 }` | `200 { "ready": true, "edge_state": "idle", "max_faces": 4, "countdown_sec": 10 }` |
| `session_start` | `POST /v1/sessions` | `{ "session_id", "started_at_ms" }` | `200 { "ok": true }`。同じ `session_id` の再送は状態を初期化する |
| `frame` | `POST /v1/sessions/{session_id}/frames` | ヘッダ `X-Frame-Id`, `X-Capture-Ms`, `X-Servo-X`, `X-Servo-Y`, `X-Width`, `X-Height`, `X-Format`, `X-Phase` (`compose` / `capture`)。本文は画像バイト列 | `200` frame_result（下記） |
| `session_timeout` | `POST /v1/sessions/{session_id}/timeout` | なし | `200 { "candidate": { "frame_id", "score", "reason" } }` または `{ "candidate": null }` |
| `review_decision` | `POST /v1/sessions/{session_id}/review` | `{ "decision": "save" \| "retake" }` | save: `202 { "status": "uploading" }`。retake: `200 { "ok": true }`（保持フレームを破棄） |
| （REVIEW 表示用） | `GET /v1/sessions/{session_id}/candidate` | なし | `200 image/jpeg`（採用フレームがあればそれ、無ければ最良候補。品質 80、サイズは受信フレームのまま。`Cache-Control: no-store`）。どちらも無ければ `404 { "error": "no_candidate" }` |
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

### review と photo

- `accepted: true` のあと device は `POST …/review {decision:"save"}` を送る。edge は採用フレームを保持するだけで、自動ではアップロードしない。`GET …/photo` は save の後にポーリングする。
- 時間切れのときは `POST …/timeout` で候補を受け取り、利用者の選択に応じて `save` か `retake` を送る。
- REVIEW で候補を見せるとき、device は `GET …/candidate` で edge が保持している同じフレームを JPEG で受け取って表示する（device 側の保持フレームと edge の候補がずれないようにする）。cancel 後・公開後・候補なしは 404 `no_candidate`。
- `expires_at` は ISO 8601 で、日本時間のオフセット `+09:00` 付き（例 `2026-09-30T22:00:00+09:00`）。

## エラー応答

エラーのときの本文は `{ "error": "<code>" }`。

| status | code | 条件 |
| --- | --- | --- |
| 400 | `bad_request` | JSON 本文が不正（必須項目の欠け、`decision` が `save` / `retake` 以外など） |
| 400 | `unsupported_protocol_version` | hello の `protocol_version` が 1 以外 |
| 400 | `invalid_session_id` | session_start の `session_id` が UUID バージョン 4 (8-4-4-4-12 形式) でない |
| 400 | `missing_header:<name>` / `invalid_header:<name>` | frame のヘッダが無い・数値でない・`X-Format` / `X-Phase` が既定値以外 |
| 400 | `empty_frame` / `bad_image` | frame の本文が空、宣言した形式・サイズでデコードできない、`X-Width` / `X-Height` が 0 以下、または JPEG の実サイズが宣言と違う |
| 400 | `image_too_large` | 宣言サイズ (`X-Width` / `X-Height`) または JPEG の実サイズが edge 設定の `max_width` / `max_height` (初期 1280×960) を超える |
| 401 | `unauthorized` | `X-Device-Id` / `X-Device-Key` の不一致（本文の検証より先に判定） |
| 404 | `unknown_session` | 未知の `session_id` |
| 404 | `no_candidate` | candidate で、採用フレームも候補も無い（顔なし・cancel 後・公開後） |
| 409 | `nothing_to_save` | 採用フレームも候補も無い、または REVIEW 以外の状態での save |
| 409 | `cancelled` | cancel 済みセッションへの timeout |
| 409 | `nothing_to_retake` | REVIEW / TIMEOUT 以外 (撮影中・アップロード中・公開後・cancel 後) での retake |
| 411 | `length_required` | frame に `Content-Length` が無い (chunked 転送は受け付けない) |
| 413 | `frame_too_large` | frame の `Content-Length` が 2 MiB を超える (読む前に判定)、または読んだ本文が 2 MiB を超えた |

## タイムアウトと再試行

- device は各リクエストに 3 秒のタイムアウトを置く。失敗しても同じフレームを再送しない（次のフレームを送る）。
- `review` の save だけは同一 `session_id` で最大 3 回まで再試行できる（edge 側は冪等）。
- edge は 5 分以上イベントの無いセッションを破棄する。
- edge が保持するセッションは最大 `max_sessions`（初期 8）。超えると一番長くイベントの無いセッションを破棄する（以後そのセッションは 404）。
- edge に繋がらない間、device は「PC未接続」を出し、自動判定つきの撮影を始めない（タッチによる撮影は固定カメラで続けられる）。
