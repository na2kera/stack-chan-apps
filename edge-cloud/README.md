# edge-cloud

edge (`edge/`) を PC ではなく Cloudflare Containers で動かすための Worker です。Worker が device の鍵を確かめてから、1 台だけ動かす edge コンテナへリクエストをそのまま渡します。設計は [docs/design/step5-edge-cloud.md](../docs/design/step5-edge-cloud.md) と [docs/design/step6-cloud-device.md](../docs/design/step6-cloud-device.md) を参照してください。

デプロイ先は `https://stackchan-edge.na2kera.workers.dev` です。いまは webcam の疑似デバイス (`edge/tools/webcam_device.py`) で計測する段階で、**K151 のファームはまだこの edge に繋げません** (HTTPS と JPEG フレームはステップ 6b / 6c で対応します)。計測結果は [docs/measurements/](../docs/measurements/) にあります。

## 必要なもの

- Node.js 22 (この Mac では `export PATH=~/.nvm/versions/node/v22.23.2/bin:$PATH`。volta 既定の 20 では wrangler が動きません)
- **Cloudflare の Workers Paid プラン** ($5/月)。Containers は有料プランでのみ使えます (Free では `wrangler containers list` もデプロイも拒否されます)
- `wrangler deploy` を実行するマシンで動いている Docker。wrangler が `edge/Dockerfile` をビルドして Cloudflare のレジストリに push します
- 公開 gallery (`gallery/`) がデプロイ済みであること。その URL と `GALLERY_KEY` を使います

Containers は linux/amd64 のイメージしか動かせません。wrangler は `--platform linux/amd64` でビルドするので、Apple Silicon の Mac でもそのままデプロイできますが、エミュレーションになるため初回のビルドと push は時間がかかります (2026-10-10 の初回は約 33 分)。

## 鍵の生成

`EDGE_DEVICE_KEY` は device (K151 / 疑似デバイス) と共通の鍵、`GALLERY_KEY` は gallery の Worker と同じ鍵です。鍵は `edge-cloud/.dev.vars` (gitignore 済み) に置き、Git・ログ・チャットに出しません。

```console
cd edge-cloud
umask 077
printf 'EDGE_DEVICE_KEY=%s\nGALLERY_KEY=%s\n' "$(openssl rand -hex 32)" "<gallery/.dev.vars の GALLERY_KEY>" > .dev.vars
```

Secret は stdin か `--secrets-file` で渡し、コマンドラインの引数に値を書きません。

```console
# どちらか
npx wrangler deploy --secrets-file .dev.vars
printf '%s' "$(grep ^EDGE_DEVICE_KEY= .dev.vars | cut -d= -f2)" | npx wrangler secret put EDGE_DEVICE_KEY
```

## デプロイ

```console
cd edge-cloud
npm ci
npx wrangler login
npx wrangler deploy --secrets-file .dev.vars   # 初回。2 回目からは Secret を変えない限り npx wrangler deploy
```

`wrangler.jsonc` の `vars.GALLERY_URL` に公開 gallery の URL を書いてあります (秘密ではありません)。空のまま (または `GALLERY_KEY` が未設定) だと、認証を通った `/v1/*` のリクエストが 503 `misconfigured` になり、コンテナは起動しません (gallery が無いと edge は mock に戻り、スマホで開けない QR を出してしまうため)。

Worker のコードだけを変えたときは `npx wrangler deploy --containers-rollout=none` でイメージのビルドを省けます。このとき動いているコンテナはそのまま動き続け、撮影中のセッションも残ります。イメージが変わったデプロイ (コンテナの rollout) では、動いているコンテナが止まって新しいイメージで起動し直し、撮影中のセッションと候補は失われます (約 7 秒の断。device は `unknown_session` で撮り直しになります)。どちらも 2026-10-10 に確認しました。

`"observability": { "enabled": true }` で Worker とコンテナのログがダッシュボード (Workers & Pages → stackchan-edge → Observability) に残ります。手元では `npx wrangler tail stackchan-edge --format pretty` で見られます。

## 動作確認

```console
curl -s -X POST https://stackchan-edge.na2kera.workers.dev/v1/hello \
  -H 'X-Device-Id: stackchan-01' -H "X-Device-Key: $EDGE_DEVICE_KEY" \
  -H 'Content-Type: application/json' -d '{"device_id":"stackchan-01","protocol_version":1}'
# → {"ready":true,"edge_state":"idle","max_faces":4,"countdown_sec":10}
```

Worker が自分で返す応答 (どれもコンテナを起こしません):

| 条件 | 応答 |
| --- | --- |
| `/v1/` 以外のパス | 404 `not_found` |
| 鍵なし・鍵違い | 401 `unauthorized` |
| `GALLERY_URL` / `GALLERY_KEY` の不備 (認証の後に判定) | 503 `misconfigured` |
| frame (`POST /v1/sessions/{id}/frames`) に `Content-Length` が無い | 411 `length_required` |
| frame の `Content-Length` が十進数でない (負を含む) | 400 `invalid_header:content-length` |
| frame の `Content-Length` が 2 MiB を超える | 413 `frame_too_large` |
| コンテナへの転送で例外 | 502 `upstream_error` |

frame の 3 つは LAN の edge (`edge/src/edge/api.py`) と同じ契約です。ただし実際には、Cloudflare の前段が chunked の本文を受け取ってから `Content-Length` を付けて Worker に渡し、数字でない `Content-Length` は Worker に届く前に Cloudflare が 400 (HTML) で返します。Worker の 411 / 400 はその前提が崩れたときのための保険で、単体テストで確かめています。実際に読んだ本文の上限は edge が確かめます。

## ログ

Worker は全応答でちょうど 1 件、次の JSON を出します。鍵・URL・セッション ID・ヘッダ値・画像は出しません。

```json
{"event":"forward","kind":"frame","status":200,"ms":64,"colo":"NRT"}
```

- `event`: `early` (Worker が自分で返した) / `forward` (コンテナの応答) / `forward_error` (転送で例外、502)
- `kind`: `hello` / `frame` / `candidate` / `save` (`POST …/review`。retake も含む) / `photo` / `other`
- `ms`: 応答ヘッダが揃うまで。Workers の時計は I/O の間だけ進むので、`early` はほぼ 0 になります
- `colo`: 受けた Cloudflare のデータセンター

クライアントが切断してランタイムが打ち切ったリクエスト (Observability で outcome が `canceled`) は、応答を返していないのでこのログが出ません。

コンテナ (edge) のログには従来どおり `device_id` と `session_id` が出ます。どちらも秘密ではありません (`device_id` は固定の識別子、`session_id` はランダムな UUID で写真 URL のトークンとは紐づかず、デバッグに使います)。鍵・写真 URL・トークン・画像は Worker とコンテナのどちらのログにも出しません。

## 鍵や設定を変えたとき (ローテーション)

Worker は Secret と `vars` をコンテナの**起動時にだけ**環境変数として渡します (`@cloudflare/containers` は動いているコンテナには `envVars` を渡し直しません)。Worker だけのデプロイでは動いているコンテナは再起動されません。device は app を開いている間 5 秒ごとに hello を送り、そのたびにスリープまでの時間が延びるので、古い値のまま動き続けます。

鍵を替える手順:

1. 新しい鍵を作って `.dev.vars` を書き換え、`npx wrangler deploy --containers-rollout=none --secrets-file .dev.vars` (または `wrangler secret put` に stdin で渡す)
2. device の app を閉じ、疑似デバイスも止めてリクエストを止める
3. 5 分待つ (`sleepAfter`。コンテナが止まる)。`npx wrangler containers list` の LIVE INSTANCES が 0 になったことを確かめる
4. K151 の `config_local.h` を新しい鍵に書き換えて書き込み、再開する。最初のリクエストで新しい値のコンテナが起動する

新しい鍵を put した瞬間から Worker は新しい鍵で認証するので、2〜4 の間は古い鍵の device は 401 になります。

## コールドスタート

コンテナは止まっている状態から、最初の認証済みリクエストで起動します。edge はモデルを読み込んでから listen するので、それまでのリクエストは待たされるか、エラー (500 / 503) になります。2026-10-10 の計測では停止状態からの hello が約 5 秒で 200 になりました。クライアントが途中で切断してもコンテナの起動は続きます ([計測記録](../docs/measurements/step6-2026-10-10.md))。

疑似デバイスは `--wait-ready-sec 120` を付けると、起動時の hello を 200 が返るまで繰り返します。

## webcam で試す

```console
cd edge
export EDGE_DEVICE_KEY=$(grep ^EDGE_DEVICE_KEY= ../edge-cloud/.dev.vars | cut -d= -f2)
uv run python tools/webcam_device.py --edge https://stackchan-edge.na2kera.workers.dev \
  --device-id stackchan-01 --candidate-via-edge --open
```

操作は [edge/README.md](../edge/README.md) の「webcam で試す」と同じです。写真 URL は gallery の公開 URL になります。`--candidate-via-edge` で、時間切れの候補を device と同じく edge の `GET …/candidate` から取って表示します。

## 計測手順

疑似デバイスを無人で回し、件数つきの p50 / p95 / max を JSON に書きます (`--headless --auto-capture`)。ウィンドウを出さず、撮影 (COMPOSE 5 秒 + CAPTURE 10 秒) → 候補の取得 → cancel を `--duration-sec` の間繰り返し、5 秒ごとに hello を送ります。`--auto-save` を付けると候補を gallery に保存します (カメラに写った人の写真が公開 gallery に上がるので、付けるときは写る人の了解を取ってください)。

```console
cd edge
export EDGE_DEVICE_KEY=$(grep ^EDGE_DEVICE_KEY= ../edge-cloud/.dev.vars | cut -d= -f2)
for fmt in rgb565 jpeg; do
  uv run python tools/webcam_device.py --edge https://stackchan-edge.na2kera.workers.dev \
    --format $fmt --candidate-via-edge --headless --auto-capture --duration-sec 600 \
    --wait-ready-sec 120 --stats /tmp/step6-$fmt.json
done
```

JSON に入るもの (`--stats`):

- `cold_hello`: 最初の hello から 200 までの時間、試行回数、各応答 (コンテナが止まっている状態から始めれば cold start)
- `hello`: warm の hello の RTT
- `frames.<format>`: 送信を試みた件数・成功件数・失敗率、RTT、`latency_ms` (edge 内の解析時間)、本文バイト数、送信間隔と fps (`p50`、遅い側の `p95_slow`、`effective` = 成功件数 / 撮影時間)
- `candidate`: `GET …/candidate` の RTT、バイト数、`Content-Length` 付きか chunked か
- `save`: `review save` から `photo_ready` までの時間
- `sessions` / `events`: 撮影の結果と、セッションを失った (`unknown_session`) 時刻

結果は `docs/measurements/step6-<date>.md` に、条件 (回線、format、`instance_type`、cold / warm、件数) と一緒に残します。コンテナの CPU / メモリ / 起動時間はダッシュボード (Workers & Pages → stackchan-edge → Containers) から転記します。

`instance_type` は `standard-2` (1 vCPU / 6 GiB) を初期値にしています。値はステップ 6d で決めます。

## 費用

**app を開いたまま放置すると課金が続きます。** device は app を開いている間 5 秒ごとに hello を送るので、コンテナは起きたままになります。疑似デバイスも同じです。使い終わったら app を閉じ、疑似デバイスを止めてください。最後のリクエストから 5 分 (`src/index.ts` の `sleepAfter`) でコンテナが止まり、止まっている間は課金されません。

コンテナは動いている間 (10 ms 単位) 課金されます。メモリとディスクはプロビジョニングした量 (`standard-2` なら 6 GiB / 12 GB) に、CPU は実際に使った量に対して課金されます。Workers Paid の基本料 $5/月に含まれる無料枠を超えた分が従量課金です。想定 (月 30 回 × 15 分、`standard-2`) では超過は月 $0.2 程度です (設計書 §3.1)。

## テスト

```console
npm test
npm run typecheck
npx wrangler deploy --dry-run --containers-rollout=none --outdir dist   # 設定の検査とバンドル (Docker 不要)
```

テストは認証・ルーティング・Content-Length の検査・アクセスログ (`src/proxy.ts`) で、Docker もデプロイも要りません。`edge/Dockerfile` のビルドと起動は CI (`edge-image` ジョブ) で確かめています。
