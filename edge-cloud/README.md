# edge-cloud

edge (`edge/`) を PC ではなく Cloudflare Containers で動かすための Worker です。Worker が device の鍵を確かめてから、1 台だけ動かす edge コンテナへリクエストをそのまま渡します。設計は [docs/design/step5-edge-cloud.md](../docs/design/step5-edge-cloud.md) を参照してください。

いまは webcam の疑似デバイス (`edge/tools/webcam_device.py`) で遅延を測るための段階です。**K151 のファームはまだこの edge に繋げません** (HTTPS と JPEG フレームが必要で、次のステップで対応します)。

## 必要なもの

- Node.js 22
- Cloudflare の Workers Paid プラン (Containers は有料プランでのみ使えます)
- `wrangler deploy` を実行するマシンで動いている Docker。wrangler が `edge/Dockerfile` をビルドして Cloudflare のレジストリに push します
- 公開 gallery (`gallery/`) がデプロイ済みであること。その URL と `GALLERY_KEY` を使います

Containers は linux/amd64 のイメージしか動かせません。wrangler は `--platform linux/amd64` でビルドするので、Apple Silicon の Mac でもそのままデプロイできますが、エミュレーションになるため初回のビルドは時間がかかります。

## デプロイ

```console
cd edge-cloud
npm ci
npx wrangler login
npx wrangler secret put EDGE_DEVICE_KEY   # device (疑似デバイス) と共通の鍵
npx wrangler secret put GALLERY_KEY       # gallery の Worker と同じ鍵
```

`wrangler.jsonc` の `vars` に gallery の URL を設定します。空のまま (または `GALLERY_KEY` が未設定) だと、認証を通った `/v1/*` のリクエストが 503 `misconfigured` になり、コンテナは起動しません (gallery が無いと edge は mock に戻り、スマホで開けない QR を出してしまうため)。認証の無いリクエストは 401、`/v1/` 以外は 404 のままです。

```jsonc
"vars": {
  "DEVICE_ID": "stackchan-01",
  "GALLERY_URL": "https://stackchan-gallery.<account>.workers.dev"
}
```

```console
npx wrangler deploy
```

鍵には十分長いランダム値を使い、Git には保存しません。Worker はこれらの値をコンテナの環境変数 `EDGE_DEVICE_KEY` / `EDGE_DEVICE_ID` / `GALLERY_KEY` / `GALLERY_URL` として渡します (イメージには入れません)。初回のデプロイ直後はコンテナの配置に数分かかり、その間は 503 が返ることがあります。

## 動作確認

```console
curl -s -X POST https://stackchan-edge.<account>.workers.dev/v1/hello \
  -H 'X-Device-Id: stackchan-01' -H "X-Device-Key: $EDGE_DEVICE_KEY" \
  -H 'Content-Type: application/json' -d '{"device_id":"stackchan-01","protocol_version":1}'
# → {"ready":true,"edge_state":"idle","max_faces":4,"countdown_sec":10}
```

鍵が違えば 401 `unauthorized`、`/v1/` 以外のパスは 404 `not_found` です。どちらもコンテナを起こしません。フレームは `Content-Length` が 2 MiB を超えると宣言していれば Worker の時点で 413 `frame_too_large` になります。実際に読んだ本文の上限は edge が確かめます。

## 鍵や設定を変えたとき

Worker は Secret と `vars` をコンテナの**起動時にだけ**環境変数として渡します (`@cloudflare/containers` は動いているコンテナには `envVars` を渡し直しません)。`wrangler secret put` や `vars` の変更、`wrangler deploy` で動いているコンテナが再起動されるかどうかは確認できていません。device は app を開いている間 5 秒ごとに hello を送り、そのたびにスリープまでの時間が延びるので、古い値のまま動き続けることがあります。

確実なのは、device の app を閉じる (疑似デバイスも止める) などしてリクエストを止め、5 分後にコンテナがスリープしてから次のリクエストを送ることです。これより早く再起動する方法は確認していません (wrangler 4.147 の `wrangler containers` にはインスタンスを止めるコマンドがありません)。

## コールドスタート

コンテナは止まっている状態から、最初の認証済みリクエストで起動します。edge はモデルを読み込んでから listen するので、それまでのリクエストはエラー (500 / 503) になるか、待たされます。device は hello が通るまで「PC未接続」を出し、通れば自動判定つきの撮影に進みます。

疑似デバイスは起動時の hello が失敗すると終了するので、先に上の `curl` を 200 が返るまで繰り返してから起動してください。

## webcam で試す

```console
cd edge
uv run python tools/webcam_device.py --edge https://stackchan-edge.<account>.workers.dev \
  --device-id stackchan-01 --key "$EDGE_DEVICE_KEY" --open
```

操作は [edge/README.md](../edge/README.md) の「webcam で試す」と同じです。写真 URL は gallery の公開 URL になります。

## 測るもの

- 往復時間: 1 フレームを送ってから結果が返るまで。hello だけなら `curl -w '%{time_total}\n' -o /dev/null ...` で目安を取れます
- `latency_ms`: frame_result に入る edge 内の解析時間。往復時間との差がネットワークと Worker の分
- fps: 疑似デバイスは前のフレームの結果を待ってから次を送るので、往復時間が `1 / --fps` を超えると実効 fps が落ちます
- コールドスタートで hello が 200 になるまでの時間

`instance_type` は `standard-2` (1 vCPU / 6 GiB) を初期値にしています。`latency_ms` を見て `standard-1` に下げるか、`standard-3` 以上に上げるかを決めてください。

## 費用

コンテナは動いている間 (10 ms 単位) だけ課金されます。device は app を開いている間 5 秒ごとに hello を送るので、その間は起きたままです。最後のリクエストから 5 分 (`src/index.ts` の `sleepAfter`) で止まり、止まっている間は課金されません。Workers Paid の無料枠を超えた分が従量課金になります。

## テスト

```console
npm test
npm run typecheck
npx wrangler deploy --dry-run --containers-rollout=none --outdir dist   # 設定の検査とバンドル (Docker 不要)
```

テストは認証とルーティング (`src/proxy.ts`) だけで、Docker もデプロイも要りません。`edge/Dockerfile` のビルドと起動は CI (`edge-image` ジョブ) で確かめています。ログ (`npx wrangler tail`) には鍵・トークン・画像を出しません。
