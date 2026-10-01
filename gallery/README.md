# gallery

Cloudflare Workers と R2 で、採用した写真を 1 時間だけ公開するサービスです。edge から共有鍵つきで JPEG を受け取り、写真保存ページと X 投稿画面への共通リンクを返します。

## デプロイ

Node.js 22 を使います。Cloudflare API token でログインする場合は、Workers の編集権限に加えて R2 の書き込み権限 (`r2:write`) を付けてください。

```console
cd gallery
npm ci
npx wrangler login
npx wrangler r2 bucket create stackchan-gallery
npx wrangler secret put GALLERY_KEY
npx wrangler deploy
```

`GALLERY_KEY` には十分長いランダム値を設定し、edge 側の同名環境変数にも同じ値を設定します。Git には保存しません。公開 URL が確定している場合は `wrangler.jsonc` の `PUBLIC_BASE_URL` に末尾 `/` なしで指定します。空文字のままなら、リクエスト URL の origin を使います。

## edge の設定

`edge/config.toml` の gallery を次のように設定して edge を起動します。

```toml
[gallery]
mode = "http"
url = "https://stackchan-gallery.<account>.workers.dev"
ttl_minutes = 60
public_base_url = ""
```

```console
cd edge
EDGE_DEVICE_KEY=<device と共通の鍵> \
GALLERY_KEY=<Worker と共通の鍵> \
uv run edge
```

`ttl_minutes` は mock 用です。HTTP gallery の保存期間は Worker 側の `TTL_MINUTES` で決まります。

## テストと TTL 確認

```console
npm test
npm run typecheck
```

期限切れを短時間で確認するときは、`wrangler.jsonc` の `TTL_MINUTES` を一時的に `"2"` に変更して deploy します。確認後は `"60"` に戻してください。写真のダウンロードでは期限は延長されず、期限後は直ちに 410、Cron により 5 分以内を目安に R2 から削除されます。
