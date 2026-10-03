# edge

PC で動かす Python サービス。device (K151) から受け取ったフレームを MediaPipe Face Landmarker で解析し、
採用判定 (spec §6.2)・首振り量 (§6.3)・時間切れ候補 (§6.4)・レビューとアップロードを行う。
通信契約は [docs/protocol.md](../docs/protocol.md)、設計は [docs/design/step2a-edge.md](../docs/design/step2a-edge.md)。

gallery は LAN 内確認用モックと、公開 HTTPS の Cloudflare Workers 版を切り替えられる。音声起動はまだ無い。

## 準備

必要なもの: [uv](https://docs.astral.sh/uv/) (0.9 系で確認)。Python 3.11 は uv が用意する。

```console
cd edge
uv python install 3.11        # 未導入なら
uv sync                       # 依存を uv.lock どおりに入れる
uv run python tools/download_model.py   # models/face_landmarker.task (float16) を取得
cp config.example.toml config.toml      # 必要なら値を編集。config.toml はコミットしない
```

`config.toml` が無いと `config.example.toml` を読んで警告を出す。
device の鍵は `config.toml` の `[auth] device_key` か環境変数 `EDGE_DEVICE_KEY` (こちらが優先) に置く。HTTP gallery の共有鍵は環境変数 `GALLERY_KEY` にだけ置く。

## 起動

```console
cd edge
EDGE_DEVICE_KEY=<device の config.h と同じ鍵> uv run edge
# オプション: --config path/to/config.toml --host 192.168.x.y --port 8765 --debug
```

起動時にモデルを 1 回ロードし、`http://<host>:8765` で listen する。ログは 1 行 1 イベントの JSON を標準エラーに出す
(画像バイト列・トークン・写真 URL・鍵は出さない)。

動作確認:

```console
curl -s -X POST http://127.0.0.1:8765/v1/hello \
  -H 'X-Device-Id: stackchan-01' -H "X-Device-Key: $EDGE_DEVICE_KEY" \
  -H 'Content-Type: application/json' -d '{"device_id":"stackchan-01","protocol_version":1}'
# → {"ready":true,"edge_state":"idle","max_faces":4,"countdown_sec":10}  鍵が違うと 401
```

## webcam で試す (K151 なし)

`tools/webcam_device.py` が device の代わりに PC の webcam で同じ HTTP フローを回す。edge を起動した状態で別ターミナルから:

```console
cd edge
uv run python tools/webcam_device.py --edge http://127.0.0.1:8765 \
  --device-id stackchan-01 --key "$EDGE_DEVICE_KEY" --open
```

1. ウィンドウに "press SPACE to start" が出たら SPACE。
2. 5 秒の COMPOSE (全員カメラに入る) → 10 秒の CAPTURE。画面に `faces / target / in_frame / eyes / smile / servo_dx,dy / 残り秒` が出る。
3. 全員が目を開けて笑い、条件を 2 フレーム連続で満たすと "captured!" → 保存 → 写真 URL を標準出力に出す (`--open` でブラウザを開く)。
4. 時間切れなら候補と理由を表示。`s` で保存、`r` で撮り直し。`q` / ESC で終了。

`--format rgb565` で device と同じ RGB565 (QVGA) を送る (`--byte-order` は edge の `rgb565_byte_order` と合わせる)。
`servo_dx/dy` は webcam では動かせないので表示だけ。macOS では初回にターミナルへのカメラ許可が必要。

## 設定 (config.toml)

| セクション | キー | 初期値 | 意味 |
| --- | --- | --- | --- |
| server | host / port | 0.0.0.0 / 8765 | listen アドレス。LAN 内の PC アドレスを推奨 (0.0.0.0 は開発時のみ) |
| server | max_sessions | 8 | 同時に保持するセッション数。超えたら一番長くイベントの無いセッションを追い出す (`session_evicted` をログに出す) |
| auth | device_id / device_key | stackchan-01 / change-me | device の `X-Device-Id` / `X-Device-Key`。`EDGE_DEVICE_KEY` が優先 |
| capture | max_faces | 4 | 最大人数 (判定と UI の上限)。MediaPipe は max_faces + 1 人まで検出し、超えたら `too_many` で採用しない |
| capture | countdown_sec | 10 | hello で device に返す撮影秒数 |
| capture | margin_ratio | 0.08 | 上下左右の安全余白 (画像比) |
| capture | min_face_width_ratio | 0.08 | これより幅の小さい顔は採用しない |
| capture | eye_blink_max | 0.25 | `eyeBlinkLeft/Right` が左右ともこれ以下で開眼 |
| capture | mouth_smile_min | 0.55 | `mouthSmileLeft/Right` が左右ともこれ以上で笑顔 |
| capture | stable_frames | 2 | 人数が連続で同じであることを要求するフレーム数 |
| capture | accept_consecutive | 2 | 条件達成がこの回数連続したフレームを採用 |
| capture | rgb565_byte_order | little | RGB565 のバイト順 (`little` / `big`)。実機で確認 |
| capture | max_width / max_height | 1280 / 960 | フレームの上限。宣言サイズ (`X-Width` / `X-Height`) か実サイズが超えたら 400 `image_too_large` |
| head | gain_x / gain_y | -0.05 / 0.05 | 画像のずれ (px) → サーボ (1/10 度)。符号は実機で校正 |
| head | deadband_px | 16 | 中心からのずれがこれ以下なら動かさない |
| head | step_max | 30 | 1 回の指示の上限 (1/10 度) |
| head | min_interval_ms | 500 | 指示の最小間隔 |
| head | search_step | 20 | 顔が無いときの探索幅 (COMPOSE のみ、往復 2 回まで) |
| head | x_min / x_max / y_min / y_max | -250 / 250 / 250 / 650 | 可動域。device の config.h と同じ値にする |
| analysis | model_path | models/face_landmarker.task | config ファイルのあるディレクトリからの相対パス |
| gallery | mode | mock | LAN 内モックは `mock`、公開 gallery は `http` |
| gallery | ttl_minutes | 60 | 写真の保存期間。期限後は 410。**mock のみ** (`http` モードは `gallery/wrangler.jsonc` の `TTL_MINUTES`) |
| gallery | public_base_url | "" | 写真 URL の先頭。空なら `http://<listen host>:<port>` (0.0.0.0 のときは推定した LAN アドレス) |
| gallery | url | "" | `http` モードの公開 gallery URL (`https://` 必須)。共有鍵は環境変数 `GALLERY_KEY` から読む |
| share | text | @na2kera_0510 のｽﾀｯｸﾁｬﾝに撮ってもらいました！　#ｽﾀｯｸﾁｬﾝ #StackChan #STECHFES2026 #STECH | X 投稿画面に入れる本文。**mock のみ** (`http` モードは `gallery/wrangler.jsonc` の `SHARE_TEXT`) |

## モック gallery について

`gallery.mode = "mock"` では edge 自身が `/mock/p/<token>` (写真ページ)、`/mock/p/<token>.jpg`、`/mock/share/x` (X 投稿画面へ 302) を配る。
**モックは LAN 内の動作確認用で、QR でスマホに配る用途には使わない** (spec 仮定 B: PC の LAN アドレスを QR に入れない)。
写真はメモリにだけ置き、edge を止めると消える。公開 HTTPS の配布はステップ4の `gallery/` で行う。

公開 gallery を使う場合は `mode = "http"` と `url = "https://stackchan-gallery.<account>.workers.dev"` を設定し、Worker と同じ共有鍵を `GALLERY_KEY` に設定する。HTTP 接続エラーと 5xx は短い待機を挟んで最大 3 回試行する。

## テスト

```console
uv run ruff check .
uv run pytest                          # モデル・カメラ・K151 なしで通る
uv run python tests/manual_mediapipe.py [face.jpg ...]   # モデルがあるときの手動確認 (pytest 対象外)
```

顔写真の実サンプルはコミットしない。
