# ステップ4 設計: gallery（写真配布）を Cloudflare Workers + R2 で作る

対象: `docs/spec.md` §7（一時保存と 2 種類の QR）と §11 の実装順 4。
edge の `MockGallery`（LAN 内モック）を、公開 HTTPS で動く本物に置き換える。device 側は変更不要（photo_url / share_url を QR にするだけ）。

## 1. ゴールと非ゴール

ゴール:

- edge から `POST /internal/photos` で JPEG を受け取り、`photo_id`・閲覧トークン・`expires_at`・公開 URL を返す。同じ `session_id` の再送は同じ写真を返す（冪等）。
- スマホが Wi-Fi でもモバイル回線でも `https://<host>/p/<token>` を開け、写真の表示・ダウンロード・削除予定時刻（JST）を見られる。
- `https://<host>/share/x` が X の投稿画面（文言 + ハッシュタグ入り）へ 302 する。写真 URL は本文に入れない。
- TTL（初期 60 分）を過ぎたら画像とページを 410 にし、5 分ごとの Cron で実体を消す。
- edge の `[gallery] mode = "http"` でこのサービスを使えるようにする。

非ゴール:

- 認証つき閲覧、アルバム、複数写真、X への自動添付（spec の MVP 境界）。

## 2. 技術選定（固定値）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| 実行環境 | Cloudflare Workers（TypeScript、`wrangler` 4.x） | ユーザー指定。無料枠で HTTPS（`*.workers.dev`）が付く |
| 保存 | R2 バケット 1 つ（`stackchan-gallery`）。写真は `photos/<token>.jpg`、`session_id` → token の索引は `sessions/<session_id>` | spec §3「DB なしでも期限情報を永続化」。期限は R2 の `customMetadata.expires_at` に持つ |
| 期限切れ削除 | Cron Trigger `*/5 * * * *` で `photos/` を list し、`expires_at` を過ぎたものと対応する `sessions/` を delete。配信時もアプリ層で期限を判定する | spec §7.1 |
| ルーティング | 素の `fetch` ハンドラ（依存を増やさない。ルートは 4 本） | |
| テスト | `vitest` + `@cloudflare/vitest-pool-workers`（miniflare で R2 と Cron を模擬） | ハードウェア不要 |
| 認証 | edge → gallery は `X-Gallery-Key`（共有鍵、Worker Secret）。`crypto.subtle.timingSafeEqual` 相当で比較 | spec §7.1 |
| トークン | `crypto.getRandomValues` 16 バイト → base64url（22 文字、128 ビット） | spec §7.1 |
| 公開ホスト | まず `https://stackchan-gallery.<account>.workers.dev`。独自ドメインは後から `routes` で追加可 | |

## 3. ソース構成

```
gallery/
├── package.json            # wrangler, vitest, @cloudflare/vitest-pool-workers, typescript
├── wrangler.jsonc          # name, main, r2_buckets, triggers.crons, vars (TTL_MINUTES, SHARE_TEXT, PUBLIC_BASE_URL)
├── tsconfig.json
├── vitest.config.ts
├── README.md               # デプロイ手順 (wrangler login → r2 bucket create → secret put → deploy)、edge 側の設定
├── src/
│   ├── index.ts            # fetch / scheduled のエントリ。ルーティングだけ
│   ├── auth.ts             # 共有鍵の定数時間比較
│   ├── store.ts            # R2 の読み書き: putPhoto / getPhoto / findBySession / deleteExpired
│   ├── token.ts            # 128 ビット乱数トークン
│   ├── pages.ts            # 写真ページ HTML (日本語)、410 ページ
│   └── share.ts            # X intent URL の組み立て
└── test/
    ├── photos.test.ts      # POST /internal/photos (201, 401, 413, 冪等)、GET /p/<token> (200, 404, 410)、ダウンロードヘッダ
    ├── share.test.ts       # /share/x が intent に 302 し、本文に写真 URL/トークンが含まれない
    └── expiry.test.ts      # TTL 経過後 410、scheduled() が実体と索引を消す
```

## 4. API

| メソッドとパス | 認証 | 入力 | 応答 |
| --- | --- | --- | --- |
| `POST /internal/photos` | `X-Gallery-Key` | `Content-Type: image/jpeg`、本文 JPEG（最大 5 MB）、ヘッダ `X-Session-Id`（UUID v4）、`X-Captured-At`（ISO 8601） | `201 { "photo_id", "token", "photo_url", "share_url", "expires_at" }`。同じ `session_id` が未失効なら `200` で同じ内容。鍵違い 401、JPEG でない 415、大きすぎ 413、`session_id` 不正 400 |
| `GET /p/<token>` | なし | | 写真ページ HTML。無ければ 404、期限切れ 410 |
| `GET /p/<token>.jpg` | なし | `?download=1` で `Content-Disposition: attachment; filename="stackchan-<日時>.jpg"` | JPEG。404 / 410 は同上 |
| `GET /share/x` | なし | | `302` → `https://x.com/intent/tweet?text=<URL エンコードした SHARE_TEXT>` |
| `scheduled` | Cron | | 期限切れの `photos/` と `sessions/` を削除し、件数をログ |

`photo_id` はトークンと同じ値で良い（外部に出すのはトークンだけ）。`expires_at` は ISO 8601 の `+09:00`（edge と同じ形式）。

共通ヘッダ（`/p/*`）: `Cache-Control: no-store`、`Referrer-Policy: no-referrer`、`X-Robots-Tag: noindex, nofollow`、`Content-Security-Policy: default-src 'none'; img-src 'self'; style-src 'unsafe-inline'`。

## 5. 保存とライフサイクル

- `putPhoto(session_id, jpeg, captured_at)`:
  1. `sessions/<session_id>` があり、その token の写真が未失効なら既存を返す（冪等）。
  2. 無ければ token を作り、`photos/<token>.jpg` を `customMetadata = { session_id, captured_at, expires_at }`、`httpMetadata.contentType = image/jpeg` で put、続けて `sessions/<session_id>` に token を put。
- 配信時: `customMetadata.expires_at <= now` なら 410（Cron 遅延中も配信しない。spec §7.1）。
- `deleteExpired(now)`: `photos/` を `list()`（1000 件ずつ、`include: ['customMetadata']`）し、期限切れを集めて `delete([...])`。対応する `sessions/<session_id>` も消す。
- ダウンロードしても期限は延びない。再撮影による即時削除は MVP では行わない（spec §7.1 の「QR 提示後は TTL まで保持」に合わせ、常に TTL で消す）。

## 6. 写真ページ（`/p/<token>`）

- 320px 幅のスマホ前提。上から: 写真（`/p/<token>.jpg`）、「写真をダウンロード」ボタン（`?download=1`）、「削除予定: 2026-10-01 15:30（日本時間）」、注意書き「この URL を知っている人は誰でも見られます。保存はお早めに」。
- 文言はすべて日本語。edge の `MockGallery` のページと揃える（`edge/src/edge/gallery.py` を参照）。
- 410 ページ: 「この写真は削除されました」。

## 7. edge 側の変更

- `edge/src/edge/gallery.py` に `HttpGallery(base_url, key, timeout)` を追加。`upload()` は `POST /internal/photos` を最大 3 回再試行（protocol.md の save 再試行とは別に、edge → gallery のネットワーク再試行）。`delete()` は MVP では no-op（gallery に削除 API を置かない。TTL で消える）。
- `config.toml` の `[gallery] mode = "http"`、`url = "https://…workers.dev"`、鍵は環境変数 `GALLERY_KEY`。
- テスト: `httpx.MockTransport` で 201 / 200 / 401 / 5xx 再試行を確認。

## 8. 受け入れチェック

- [ ] `npm test`（vitest, miniflare）が通る。
- [ ] `wrangler deploy` で `https://stackchan-gallery.<account>.workers.dev` が立ち、`GET /share/x` が X の投稿画面に飛ぶ。
- [ ] edge から実写真を POST し、返った URL をスマホの **モバイル回線**で開いて JPEG を保存できる。
- [ ] 同じ `session_id` の再送で同じ URL が返る。鍵違いで 401。
- [ ] TTL を 2 分にして試し、期限後に 410 になり、5 分以内に R2 から消える。
- [ ] ページの `<head>` に noindex、応答に no-store と no-referrer が付く。
- [ ] Worker のログにトークン全体が出ない（出すなら先頭 4 文字まで）。
- [ ] K151 で一周し、写真 QR と X 投稿 QR が本物の URL で読める。

## 9. 設定と秘密

- `wrangler.jsonc` の `vars`: `TTL_MINUTES = "60"`、`SHARE_TEXT = "スタックチャンに撮ってもらいました！ #スタックチャン #StackChan"`、`PUBLIC_BASE_URL`（空なら `request.url` のオリジンを使う）。
- Secret: `GALLERY_KEY`（`wrangler secret put GALLERY_KEY`）。edge の `GALLERY_KEY` と同じ値。Git に入れない。
- R2 バケット名 `stackchan-gallery`（`wrangler r2 bucket create`）。
