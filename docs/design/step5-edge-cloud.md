# ステップ5 設計: edge を Cloudflare Containers で動かす

対象: `docs/spec.md` 実装仮定 B（edge は PC 上）の代替。撮影のたびに PC を用意しなくて済むよう、edge を公開 HTTPS のクラウドで動かす。
このステップは edge をデプロイできるようにし、疑似デバイス（`edge/tools/webcam_device.py`）で遅延を測るところまで。K151 のファームの対応は次のステップ。

## 1. ゴールと非ゴール

ゴール:

- `edge/` をそのままコンテナイメージにし（`edge/Dockerfile`）、Cloudflare Containers で動かす。モデルはイメージに入れる。
- 前段の Worker（`edge-cloud/`）が `X-Device-Id` / `X-Device-Key` を確かめ、通ったリクエストだけを edge コンテナに渡す。認証されていないリクエストではコンテナを起こさない。
- edge の設定は `config.example.toml` を正とし、鍵と gallery の接続先は環境変数で渡す（`EDGE_DEVICE_KEY` / `EDGE_DEVICE_ID` / `GALLERY_KEY` / `GALLERY_URL`）。
- `webcam_device.py --edge https://…` で一周でき、往復時間・`latency_ms`・fps を測れる。
- CI でイメージをビルドし、起動して hello が通ることを確かめる。

非ゴール:

- K151 のファームの変更（HTTPS クライアント、JPEG フレーム）。次のステップ。
- 音声起動（spec ステップ3）。
- 複数台・オートスケール。セッションは edge のメモリにあるので 1 台で動かす。
- protocol.md の変更。device から見た API は LAN の edge と同じ。

## 2. 技術選定（固定値）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| 実行環境 | Cloudflare Containers（Durable Object に紐づくコンテナ、linux/amd64） | edge は MediaPipe Face Landmarker の Python バインディング（ネイティブ拡張）と blendshape スコア（`eyeBlink*` / `mouthSmile*`）に依存する。Workers（V8 isolate。Python Workers も Pyodide 上で、MediaPipe のネイティブ拡張は載らない）では動かせない。gallery と同じ Cloudflare アカウントで完結する |
| インスタンス | `standard-2`（1 vCPU / 6 GiB）、`max_instances: 1` | 初期値。`latency_ms` を測って決め直す。セッション・候補フレーム・mock の写真は edge のメモリにあるので、2 台以上にするとリクエストごとに別のセッション表になる |
| 前段 | Worker（TypeScript、`wrangler` 4.x、`@cloudflare/containers`）。`/v1/*` だけを転送 | 認証をコンテナの起動前に済ませる（鍵なしのスキャンでコンテナを起こさない＝課金しない）。HTTPS（`*.workers.dev`）は Worker が終端する |
| インスタンスの選び方 | `getContainer(env.EDGE, "edge")`（名前固定の 1 台） | 上と同じ理由 |
| スリープ | `sleepAfter = "5m"` | device は app を開いている間 5 秒ごとに hello を送るので、使っている間は起きたまま、閉じて 5 分で止まる。止まっている間は課金されない |
| 認証 | Worker と edge の両方で `X-Device-Id` / `X-Device-Key` を定数時間比較。Secret 未設定なら必ず 401 | Worker は起動を防ぐため、edge は従来どおりの防御（コンテナに直接届く経路を想定しない多層化） |
| 設定の不備 | `GALLERY_URL`（https）か `GALLERY_KEY` が無ければ 503 `misconfigured`。コンテナを起こさない | gallery が無いと edge は mock に戻り、スマホで開けない QR を出す |
| フレームの上限 | `Content-Length` > 2 MiB を Worker で 413 `frame_too_large` | edge と同じ上限とエラーコード。大きな本文をコンテナまで運ばない。edge も従来どおり検査する |
| イメージ | `python:3.11-slim-trixie` + uv 0.9.9（`uv sync --locked --no-dev`）。モデルはビルド時に取得し SHA-256 を検証。非 root で 8765 を listen。`EDGE_REQUIRE_DEVICE_KEY=1` で例の鍵のままなら起動しない | CI と同じ uv と lock。apt は `import mediapipe` に要る共有ライブラリ（libEGL / libGLESv2 / libGL / glib / xcb）だけ |
| テスト | Worker は vitest（Node。認証とルーティングを `src/proxy.ts` に分けて試す）。イメージは CI でビルドして起動し、hello 200 と鍵違い 401 を確かめる | コンテナ本体は Docker とデプロイが要る |

### コールドスタート

止まっているコンテナは最初の認証済みリクエストで起動する。edge はモデルを読み込んでから listen するので、それまでのリクエストはエラーか待ちになる。device は hello が通るまで「PC未接続」を出す（protocol.md「タイムアウトと再試行」）。起動にかかる時間はこのステップで測る。

### 設定の反映

`@cloudflare/containers` は `envVars` をコンテナの起動時にだけ渡す (動いているコンテナには `startContainerIfNotRunning` が何もせずに戻る。ライブラリのソースで確認)。`wrangler secret put` や `vars` の変更で動いているコンテナが再起動されるかは未確認。device の 5 秒ごとの hello が `sleepAfter` を延ばし続けるので、古い値はコンテナが止まるまで残る。回避策は、リクエストを止めて 5 分のスリープを待つこと。より早い再起動の方法は未確認 (wrangler 4.147 の `wrangler containers` にインスタンスを止めるコマンドは無い)。

### プライバシー

LAN の edge と違い、顔の写ったフレームがインターネットを通る。経路は TLS（device → Worker）と Cloudflare 内部（Worker → コンテナ）。edge はフレームをメモリにだけ置き、ディスクにもログにも書かない（従来どおり）。保存されるのは採用して「保存する」を選んだ 1 枚だけで、gallery の TTL で消える（spec §7.1）。コンテナが止まるとメモリ上の候補も消える。

## 3. ソース構成

```
edge/
├── Dockerfile              # python:3.11-slim-trixie + uv、モデルを焼き込み、config.example.toml を config.toml に
├── .dockerignore           # 許可リスト。手元の config.toml / .venv / models / tests / tmp を送らない
└── src/edge/config.py      # EDGE_DEVICE_ID、GALLERY_URL (mode も http に) の環境変数上書きを追加。上書き後も検査する
edge-cloud/
├── package.json            # wrangler, @cloudflare/containers, vitest, typescript
├── wrangler.jsonc          # containers (image ../edge/Dockerfile, standard-2, max 1)、durable_objects、exports、vars
├── tsconfig.json
├── vitest.config.ts
├── README.md               # デプロイ手順、確認、コールドスタート、費用、測るもの
├── src/
│   ├── index.ts            # EdgeContainer (defaultPort 8765, sleepAfter 5m, envVars) と fetch の配線
│   └── proxy.ts            # /v1 の振り分け、鍵の照合、gallery 設定の確認、413。Workers 固有の import なし
└── test/
    └── proxy.test.ts       # 404 / 401 / 503 / 413 で転送しないこと、正しいリクエストをそのまま渡すこと
```

## 4. 残り（次のステップ以降）

- ファーム（`firmware/main/apps/app_photobooth/`）: HTTPS クライアント（証明書の検証込み）と JPEG フレーム。RGB565 の QVGA は 150 KiB あり、インターネット越しには重い。
- 実測: 往復時間、`latency_ms`、実効 fps、コールドスタート時間。結果で `instance_type` と device のフレーム間隔を決める。
- 音声起動（spec ステップ3）をクラウドの edge で受けるか。

## 5. 受け入れチェック

- [ ] CI の `edge-image` ジョブでイメージがビルドでき、Worker と同じ 4 つの環境変数で起動して `POST /v1/hello` が 200、鍵違いと既定 ID が 401、黒の RGB565 フレームが 200 (`face_count` 0) になる。
- [ ] `npm test` / `npm run typecheck` / `wrangler deploy --dry-run --containers-rollout=none`（`edge-cloud/`）が通る。
- [ ] `wrangler deploy` で `https://stackchan-edge.<account>.workers.dev` が立ち、`curl …/v1/hello` が（コールドスタート後に）200 を返す。
- [ ] 鍵なし・鍵違い・`/v1/` 以外のリクエストではコンテナが起動しない（ダッシュボードでインスタンスが増えない）。
- [ ] `GALLERY_URL` が空のときは 503 `misconfigured` で、コンテナが起動しない。
- [ ] `webcam_device.py --edge https://…` で一周し、返った写真 URL をスマホのモバイル回線で開ける。
- [ ] 往復時間・`latency_ms`・fps・コールドスタート時間を記録した。
- [ ] 最後のリクエストから約 5 分でコンテナが止まる。
- [ ] Worker とコンテナのログに鍵・トークン・写真 URL・画像が出ない。
- [ ] 初回デプロイで、Worker 経由の frame POST が `Content-Length` 付きで edge に届くことを `curl` で確かめる (edge は無いと 411 を返す。Worker → Durable Object → コンテナの経路で保たれるかは未確認)。
- [ ] コンテナ経由の応答の形を確かめる。ライブラリが本文を IdentityTransformStream で包み直すので、`Content-Length` ではなく chunked になる可能性が高い。ファームのステップ (HTTP クライアント) で効く。
- [ ] コールドスタートがライブラリのポート待ち (約 20 秒) を超えると、最初のリクエストは 500 になり、再試行で通ることを確かめる。matplotlib のフォントキャッシュをビルド時に作っておくのは、後で起動を縮める候補 (このステップではやらない)。
- [ ] `EDGE_DEVICE_KEY` を設定し忘れたとき、コンテナが例の鍵 (`change-me`) で listen せず終了する (イメージは `EDGE_REQUIRE_DEVICE_KEY=1`)。

## 6. 設定と秘密

- `edge-cloud/wrangler.jsonc` の `vars`: `DEVICE_ID = "stackchan-01"`、`GALLERY_URL`（公開 gallery の https URL。空なら 503）。
- Secret: `EDGE_DEVICE_KEY`（device と共通）、`GALLERY_KEY`（gallery の Worker と共通）。`wrangler secret put` で入れ、Git に入れない。
- Worker はこれらをコンテナの環境変数 `EDGE_DEVICE_ID` / `EDGE_DEVICE_KEY` / `GALLERY_URL` / `GALLERY_KEY` として渡す。調整値は `edge/config.example.toml`（イメージに `config.toml` として入る）。
