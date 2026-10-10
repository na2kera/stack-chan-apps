# ステップ6 設計: K151 をクラウドの edge に繋ぐ

対象: [step5-edge-cloud.md](step5-edge-cloud.md) §4「残り」の具体化。step5 で作った `edge-cloud/` を実際にデプロイし、K151 のファーム (`firmware/main/apps/app_photobooth/`) を PC ではなくクラウドの edge に HTTPS で繋ぎ、フレームを JPEG で送る。step5 の置き換えではない。

step5 で未完了のもの (デプロイ、webcam での計測) もこのステップに含める。ファームの変更は [.claude/skills/firmware-testable-design](../../.claude/skills/firmware-testable-design/SKILL.md) の約束 (ハードに依存しないロジックを分けてホストでテストする) に従う。

## 0. 前提 (作業前に揃えるもの)

| 項目 | 状態 (2026-10-10) | 必要な対応 |
| --- | --- | --- |
| Cloudflare のプラン | Workers Paid (2026-10-10 にユーザーが変更。`wrangler containers list` が通る) | なし。Free だと Containers が拒否される |
| wrangler のログイン | 済み (OAuth) | なし。`wrangler login` の再実行は不要 |
| Docker | 起動中 | `wrangler deploy` のとき動いていること |
| Node | `~/.nvm/versions/node/v22.23.2` (volta 既定の 20 では wrangler が動かない) | `PATH=~/.nvm/versions/node/v22.23.2/bin:$PATH` |
| gallery | `https://stackchan-gallery.na2kera.workers.dev` にデプロイ済み。`GALLERY_KEY` は `gallery/.dev.vars` | なし |
| `stackchan-edge` Worker | 未作成 | 6a で作る |

6b (ファームの HTTPS 化) は、6a を待たずに **Cloudflare Tunnel の quick tunnel** (`brew install cloudflared` のあと `cloudflared tunnel --url http://localhost:8765`。無料・アカウント不要、`*.trycloudflare.com` の正規の証明書) で PC の edge を HTTPS 公開して検証できる。quick tunnel で確かめられるのは「device → Cloudflare エッジの TLS (公開 CA の証明書、DNS、SNI)、HTTP/1.1 の持続接続、`cloudflared` を通った応答が Content-Length / chunked のどちらでも読めること」まで。Worker の認証・前処理・ログ、Durable Object とコンテナへの転送、cold start、コンテナ再起動、費用は確かめられない (6a / 6d で確かめる)。quick tunnel は開発用で SLA が無く、URL は起動ごとに変わるので本番の設定値には使わない。手元に `~/.cloudflared/config.yaml` があると quick tunnel は起動しない。

## 1. ゴールと非ゴール

ゴール:

- `edge-cloud/` を `https://stackchan-edge.<account>.workers.dev` にデプロイし、観測性 (Worker とコンテナのログ) を有効にする。
- 疑似デバイス (`edge/tools/webcam_device.py`) で cold / warm の hello、RGB565 と JPEG のフレーム、候補 JPEG の取得、gallery 保存を一周し、件数つきの p50 / p95 を記録する。
- ファームが `https://` の URL に証明書検証つきで繋がる。LAN の `http://IP:port` も引き続き使える。
- ファームがフレームを JPEG (品質 80) で送る。RGB565 はビルド時の切り替えで残す (比較と退避用)。
- 実測の結果でフレーム間隔、タイムアウト、`sleepAfter`、`instance_type` を決め、設定値を更新する。

非ゴール:

- 音声起動 (spec ステップ3)。
- 複数台・オートスケール (セッションが edge のメモリにあるため 1 台のまま)。
- `docs/protocol.md` の変更。`X-Format: jpeg` は既に契約に含まれている (protocol.md:13)。protocol version は 1 のまま。
- Content-Length なしのストリーミング送信 (edge は `Content-Length` 必須。`edge/src/edge/api.py:53`)。
- コンテナの再起動時にセッションを復元すること。
- 旧 `config_local.h` (`EDGE_HOST` / `EDGE_PORT`) をそのまま受け付けること (§3.2「接続先の設定」)。

## 2. 分割と順序

HTTPS 化と JPEG 化は同時にしない (切り分けのため)。各サブステップは別 PR、それぞれ codex レビューを通す。

| サブステップ | 内容 | 触る場所 | 検証 |
| --- | --- | --- | --- |
| 6a | edge-cloud のデプロイと観測性、Worker の前処理とログ、webcam の計測機能、計測 | `edge-cloud/`、`edge/tools/webcam_device.py`、`docs/measurements/` | PC のみ |
| 6b | ファームの HTTPS 化 (フレームは RGB565 のまま)。URL 設定、証明書バンドル、タイムアウトの分離、cold start の UI | `firmware/main/apps/app_photobooth/` (net / flow / view / config)、`firmware/sdkconfig.defaults`、`firmware/tests/` | 実機 (quick tunnel、6a が済んでいれば Worker) |
| 6c | ファームのフレーム JPEG 化 | `net/http_edge_client.cpp` (net タスク内)、`config.h`、`firmware/main/CMakeLists.txt` | 実機 |
| 6d | 統合計測と値の決定 (フレーム間隔、タイムアウト、`sleepAfter`、`instance_type`) | `config.h`、`edge-cloud/wrangler.jsonc`、`src/index.ts`、`docs/measurements/` | 実機 + ダッシュボード |

6a と 6b は独立しているので並行してよい。6c は 6b の後。6d は 6a〜6c の後。

## 3. 技術選定 (固定値)

### 3.1 クラウド側 (6a)

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| 観測性 | `wrangler.jsonc` に `"observability": { "enabled": true }` を追加 (トップレベル。コンテナのログにも効く) | Container のログをダッシュボードで見るのに必要。計測の前に入れる |
| Worker のログ | `src/index.ts` の `fetch` で `handle()` を包み、**全応答で 1 件** JSON を出す: `event` (`early` / `forward` / `forward_error`)、`kind` (`hello` / `frame` / `candidate` / `save` / `photo` / `other`)、`status`、`ms`、`colo` (`request.cf?.colo`)。転送で例外が出たら 502 `upstream_error` を返してからログ | 早期の 401 / 411 / 413 も転送の成否も同じ形で数える。鍵・画像・URL・ヘッダ値は出さない (edge の `logging_setup.py` と同じ方針)。コンテナ (edge) のログには従来どおり `device_id` と `session_id` を出す (秘密ではない。`device_id` は固定の識別子、`session_id` はランダムな UUID で写真 URL のトークンとは紐づかず、デバッグに要る)。鍵・写真 URL・トークン・画像は出さない |
| frame の `Content-Length` | edge と同じ契約に揃える: 無い → `411 length_required`、十進数でない・負 → `400 invalid_header:content-length`、2 MiB 超 → `413 frame_too_large` (現状)。どれもコンテナを起こさない。**初回デプロイで Worker → コンテナの経路で `Content-Length` が保たれることを確かめてから入れる** | LAN 直結と Worker 経由でエラー契約を変えない (`edge/src/edge/api.py:59,62`)。不要な起動と課金を防ぐ |
| 鍵 | `openssl rand -hex 32` で生成。`EDGE_DEVICE_KEY` と `GALLERY_KEY` は `wrangler secret put`。ローテーションは「新しい値を put → device の app を閉じてリクエストを止める → 5 分待つ (コンテナ停止) → 再開」 | step5「設定の反映」のとおり、動いているコンテナには新しい値が渡らない |
| `GALLERY_URL` | `wrangler.jsonc` の `vars` に `https://stackchan-gallery.na2kera.workers.dev` を書く (Git に入れてよい。秘密ではない) | 空だと 503 `misconfigured` |
| heartbeat と sleep | app を開いている間は 5 秒ごとの hello で warm を保つ (現状維持)。`sleepAfter = "5m"` のまま | IDLE で sleep させると撮影開始のたびに起動待ち (数十秒) が入る |
| 費用の前提 | 想定: 月 30 回 × 1 回 15 分 (利用 10 分 + 停止猶予 5 分) = 7.5 時間。`standard-2` (6 GiB) で 45 GiB 時間、無料枠 25 GiB 時間を 20 GiB 時間超過 ≒ $0.2/月。**6d の判断基準: 基本料 $5 に加えて月 $2 以内。超えるなら `standard-1` (4 GiB) に落とす、または `sleepAfter` を短くする** | Containers はプロビジョニングしたメモリ・ディスクに稼働時間で課金 (Cloudflare Containers pricing)。app を開いたまま放置すると課金が続く点は README に書く |
| コンテナ再起動 | ホスト再起動・rollout・OOM でコンテナが止まると、撮影中のセッションと候補は失われる。**復元しない**。device は既存のエラー経路 (`unknown_session` (`edge/src/edge/session.py:173`) / 接続断) で ERROR「接続が切れました」→ 撮り直し | 単一インスタンスでも Cloudflare は実行中コンテナの停止があり得ると明記している。復元の実装 (セッションの永続化) は割に合わない |

### 3.2 ファームの HTTPS (6b)

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| 接続先の設定 | `config_local.h` の `EDGE_HOST` / `EDGE_PORT` を **`EDGE_BASE_URL`** (例 `"https://stackchan-edge.xxx.workers.dev"` / `"http://192.168.0.167:8765"`) に置き換える。**意図的な破壊変更**: `net/edge_config.h` で `EDGE_HOST` が定義されていて `EDGE_BASE_URL` が無ければ `#error "config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました (firmware/README.md「edge と繋ぐ」)"` で止める。`config_local.example.h` と `firmware/README.md` に移行手順 (`http://<旧 EDGE_HOST>:<旧 EDGE_PORT>` と書く) を載せる | 設定ファイルは 1 つ (ユーザーの手元) しか無いので二重対応より明確なエラーのほうが安全 |
| URL の文法 (`net/edge_url`) | `scheme "://" host [":" port] ["/"]`。scheme は `http` / `https` (大小文字を区別しない。小文字に正規化)。host は DNS 名か IPv4 (1〜64 文字。IPv6 と userinfo は不可)。port は 1〜65535、省略時は http 80 / https 443。末尾の `/` は 1 つだけ許して捨てる。path・query・fragment があれば不正。不正なら `begin()` が失敗し、診断に「接続先の書式が不正」 | 既存コードの request() が path を差し替える構造 (host / port / path を別に持つ) を変えない。純粋関数にしてホストでテストする |
| transport | scheme が `https` なら `HTTP_TRANSPORT_OVER_SSL` + `crt_bundle_attach = esp_crt_bundle_attach`、`http` なら従来の `HTTP_TRANSPORT_OVER_TCP`。`esp_http_client_config_t` は `url` ではなく `host` / `port` / `path` に入れる | 上と同じ |
| 証明書検証 | **常に検証する**。`skip_cert_common_name_check` は使わない。`CONFIG_ESP_TLS_INSECURE` は無効のまま。https の接続先はホスト名で書く (IP での https は不可。`edge_url` で弾く) | SNI と CN 検証に必要 |
| 証明書バンドル | `firmware/sdkconfig.defaults` に `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` と `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL=y` (今の生成済み `sdkconfig` と同じ) を明示する | 今は生成済み `sdkconfig` にしか無い (ESP-IDF の既定値)。純正 OTA の `crt_bundle_attach` は `CONFIG_EXAMPLE_USE_CERT_BUNDLE` が未定義で使われていない (`firmware/main/hal/utils/ota/ota.c:8,140`) ので「既存の利用例」ではない。追跡対象に固定して、純正の subtree 更新で落ちないようにする |
| 時刻 | 純正は起動時に RTC からシステム時刻を復元し (`firmware/main/hal/hal_rtc.cpp:40`)、Wi-Fi 接続後に SNTP を始めるが同期を待たない (`hal_network.cpp:31-43`)。ファームは **https のときだけ、システム時刻が不正 (2025-01-01 より前) なら最初の hello の前に同期を最大 5 秒待つ**。時刻が妥当なら待たない。待ち切れなければそのまま hello を試す | RTC が妥当なら待たないので起動が遅くならない。1970 年のままだと証明書の有効期間の検査で失敗する |
| エラーの分類 | `esp_http_client_open()` の失敗は `esp_http_client_get_and_clear_last_tls_error(http, &tls_err, &cert_flags)` と `esp_http_client_get_errno(http)` で分ける: DNS 失敗 (`ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME` / `getaddrinfo` 失敗)、TCP 失敗 (errno `ECONNREFUSED` / `ETIMEDOUT` / `EHOSTUNREACH`)、証明書エラー (`cert_flags != 0`)、TLS 接続失敗 (その他の `tls_err`)。**「時刻未同期」は `cert_flags` に `MBEDTLS_X509_BADCERT_EXPIRED` か `BADCERT_FUTURE` があり、かつシステム時刻が不正なとき** だけ。時刻が妥当なら「証明書エラー」 | 現状は `ESP_ERR_HTTP_CONNECT` を一律 `kErrConnect` にしている (`http_edge_client.cpp:1367`)。expired の試験と時刻未同期の試験は別 |
| タイムアウト | 1 試行と操作全体を分ける (下表)。hello は接続後の応答待ちで切れたら `Starting` 扱い (下の UI) | TLS ハンドシェイク (ESP32-S3 で 1〜2 秒) + Cloudflare 往復 + cold start を 3 秒には収められない |
| keep-alive | 1 本の持続接続を使い回す (現状)。相手側の切断は既存の「新しい接続で 1 回だけ再送」で吸収する。TLS では再接続 = 再ハンドシェイクなので、再送 1 回の上限は維持する。Cloudflare の HTTP/1.1 keep-alive の上限は 400 秒だが、5 秒ごとの hello がある限り届かない | 既存設計 (fw-app-step2 §4.5) |
| メモリ | TLS で mbedTLS のバッファ (動的、`CONFIG_MBEDTLS_DYNAMIC_BUFFER=y`) が増える。`kTaskStack` は 8192 → **12288** を初期値にし、`logStats()` の high-water の **最小余裕 2 KiB 以上** を保てる最小値に決め直す。`kHttpRxBuf` / `kHttpTxBuf` は変えない | TLS のレコード処理は内部 RAM を使う |
| 診断画面 | `Diagnostics::edge_host[64]` / `edge_port` を **`edge_url[96]`** (scheme + host + port。path と鍵は含めない) に置き換え、`last_error` の文言を追加 (下) | 接続先が URL になるため |
| 接続状態 | `LinkState` を `Offline` / `Starting` / `Online` の 3 値にし、`flow/flow.cpp:478` の変換、`view/view.h:36` の `IdleLink`、`view/view.cpp:45` の表示も 3 値にする | 「準備中…」を出すため |
| UI の文言 | PC 前提の文言をクラウドでも通じる言い方に変える (下表)。接続先が http か https かで文言は変えない | `view/strings.h:32` 以降は全面的に PC 前提 |

タイムアウト表 (6b の初期値。6d で実測して決め直す):

| リクエスト | 1 試行の期限 | 再送 | 操作全体の上限 | 根拠 |
| --- | --- | --- | --- | --- |
| hello (接続確認) | `HELLO_TIMEOUT_MS = 8000` | 通信失敗時に 1 回 (現状の `retry_transport`) | 約 16 秒 | TLS ハンドシェイク + Worker。cold start 中は Worker がコンテナのポートを最大 20 秒待つ (`@cloudflare/containers` 0.3.7 の既定) ので、接続後の応答待ちで切れる。それは `Starting` として扱い、5 秒後の次の hello で再試行する。**6a で「クライアントが切断してもコンテナの起動が続くこと」を確認する。続かなければ Worker が `503 starting` を即返しに変える** |
| frame | `EDGE_TIMEOUT_MS = 3000` (現状) | しない (次のフレームを送る) | 3 秒 | 失敗時の上限。fps の条件は別 (§3.4: 成功サイクル p50 ≤ 500 ms) |
| candidate (SHUTTER) | `SHUTTER_CANDIDATE_TIMEOUT_MS = 800` → **1500** | しない | 1.5 秒 | インターネット往復分。save の開始遅延は 2 秒以内 (§3.4) |
| candidate (REVIEW) / save | `EDGE_TIMEOUT_MS = 3000` | save は `UPLOAD_RETRY` (現状) | 現状 | |
| アプリ終了 | `end()` は 1 秒で net タスクを切り離す (現状)。切り離されたタスクは最長 hello の 16 秒残る。その間は `begin()` を断る (「前の通信が終わっていません」) | | | 既存の制限の延長。許容する |

UI の状態 (待機画面の 1 行):

| 条件 | `LinkState` | 文言 | 今の文言 |
| --- | --- | --- | --- |
| 直近に 2xx | `Online` | 「接続中」 | 「PC接続中」 |
| DNS / TCP / TLS / 証明書で繋がらない | `Offline` | 「接続できません」 | 「PC未接続」 |
| 接続は成立 (`esp_http_client_open()` 成功) したが、hello の応答待ちで期限切れ、または Worker が 5xx (cold start、`misconfigured`、`upstream_error`) | `Starting` | **「準備中…」** (新規) | 「PC未接続」 |
| 401 | `Offline` | 「認証エラー」(診断) | 同じ |
| ERROR の `kErrNoPc` / `kErrEdgeLost` / `kNetConnectFailed` / `kNetNoResponse` | | 「PC」を外す (「接続が無いため保存できません」「接続が切れました」「接続できません」「応答がありません」) | PC 前提 |

`Starting` が続く上限は設けない (hello を 5 秒ごとに送り続ける)。診断の `last_error` に追加する文言: 「DNS失敗」「証明書エラー」「時刻未同期」「TLS接続失敗」「接続先の書式が不正」「準備中 (応答待ち)」。

### 3.3 ファームの JPEG (6c)

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| 符号化の場所 | **net タスクの中**。`offerFrame()` は従来どおり RGB565 をスロットへコピーして即 return (newest-wins を維持)。net タスクがスロットを取り出した直後に JPEG にして送る | Flow / UI / カメラタスクを待たせない。所有権がスロットにある時点で符号化すれば直列化が自然に保たれる |
| 並列性 | エンコーダは同時に 1 つだけ (net タスクが 1 つなので自然に満たす)。コメントで表明する | メモリのピークを抑える |
| エンコーダと所有権 | 純正の `image_to_jpeg()` (`firmware/xiaozhi-esp32/main/display/lvgl_display/jpg/image_to_jpeg.cpp:427`、`esp_new_jpeg`、RGB565 → YUYV 変換 → JPEG。ESP32-S3 はソフトウェア) を使う。**返ってきた出力バッファ (QVGA で常に 180,736 バイトを確保して返す。`image_to_jpeg.cpp:383,417`) を net タスクが送信完了まで所有し、`free()` する。別のバッファへコピーしない**。`image_to_jpeg_cb()` は使わない (内部で全量を確保するので減らない。`:406`) | コピーを増やさない。`Content-Length` には返ってきた長さを使う |
| JPEG のサイズ上限 | 設けない (edge の 2 MiB と出力バッファの 180 KiB が自然な上限)。6c の計測で **実写・高ノイズ入力の最大サイズ** を記録する | 「数十 KB」は保証ではない。固定上限で正常なフレームを捨てない |
| 品質 | 80 | edge の候補 JPEG と同じ実績値 |
| ピークメモリの見積り (PSRAM) | カメラ 2 面 300 KiB + プレビュー 150 KiB (`view/view.cpp:61`) + 送信スロット 2 面 300 KiB + `image_to_jpeg` の YUYV 入力 150 KiB + 出力 180 KiB + 候補 JPEG 受信 最大 256 KiB とそのデコード 150 KiB ≒ 1.5 MiB (PSRAM 8 MiB)。内部 RAM は `esp_new_jpeg` のワーク領域と TLS 分。**符号化と候補受信・TLS 再接続が重なる場面 (SHUTTER 直後) で最小空き・最大連続領域・high-water を記録する** | 見積りは上限。実測で縮める |
| 失敗時 | 符号化に失敗したフレーム (確保失敗を含む) は **破棄** する (RGB565 に落とさない)。統計ログに回数を出す | 帯域の急増を避ける |
| 切り替え | `config.h` に `FRAME_FORMAT_JPEG` (既定 1) を置き、`idf.py -DPHOTOBOOTH_FRAME_RGB565=1 build` で 0 にする。**`firmware/main/CMakeLists.txt` で CMake 変数をコンパイル定義に渡す** (既存の `PHOTOBOOTH_NO_EDGE` と同じ方法。`CMakeLists.txt:336`) | CMake 変数は渡さないと C++ から見えない |
| ヘッダ | `X-Format: jpeg`。`X-Width` / `X-Height` は 320 / 240 のまま (edge は実 JPEG の寸法と照合する。`edge/src/edge/image.py:91`) | 既存契約 |
| 統計 | `logStats()` に `jpeg_encode_ms` (平均 / 最大)、JPEG サイズ (平均 / 最大)、符号化失敗数、送信 fps を追加 | 6d の判断材料 |

### 3.4 計測 (6a / 6d)

`edge/tools/webcam_device.py` に `--stats <path>` を足し、終了時に JSON で以下を書く (件数つき)。表示は従来どおり。

- cold hello: 最初の要求から 200 までの時間、試行回数、各応答コード
- warm hello の RTT (件数、p50 / p95 / max)
- frame: 本文バイト数、RTT、`latency_ms` (件数、p50 / p95 / max)、fps は送信間隔の p50 / p95 と実効 fps (成功件数 ÷ 撮影時間)。`--format` ごとに別に取る
- `GET …/candidate`: RTT、本文バイト数、`Content-Length` 付きか chunked か
- save: `photo_ready` までの時間
- 候補表示は `--candidate-via-edge` で edge 経由の `GET …/candidate` を使う (今はローカルの画像。`webcam_device.py:254`)

結果は `docs/measurements/step6-<date>.md` に、条件 (回線、format、instance_type、cold / warm、件数) と一緒に残す。コンテナの CPU / メモリ / 起動時間はダッシュボードから転記する。

K151 では `logStats()` の 5 秒ごとのログから、`jpeg_encode_ms`、TLS 初回接続 / 再利用接続の所要、capture → frame_result の所要、high-water、内部 RAM の最小空きと最大連続領域を分けて記録する。hello の統計 (成功 / `Starting` / 失敗の回数と所要) も `logStats()` に出す。

合格基準 (6d):

| 項目 | 基準 |
| --- | --- |
| warm の送信 fps (K151、JPEG) | ≥ 2 (fw-app-step2 §5 と同じ)。符号化 + 通信 + 解析の成功 1 サイクル p50 ≤ 500 ms (件数 ≥ 500) |
| cold start から hello 200 まで | ≤ 60 秒。接続が成立してからは「準備中…」が出る |
| warm hello RTT (K151) | p95 ≤ 1500 ms (件数 ≥ 100) |
| SHUTTER の save 開始遅延 | candidate 取得込みで 2 秒以内 |
| 内部 RAM | 最小空き ≥ 40 KiB、最大連続領域 ≥ 16 KiB、かつ TLS の再接続 (試験 17) が成功する |
| net タスクのスタック | high-water の余裕 ≥ 2 KiB |
| frame の失敗率 (warm、10 分以上、送信を試みた件数 ≥ 500) | < 2 %。分母は「送信を試みたフレーム」(符号化失敗の破棄 + HTTP 失敗を分子に含む。offline で送らなかったものは含まない) |
| 費用 | §3.1 の前提で月 $2 以内 (基本料を除く) |

## 4. ソース構成 (変更するもの)

```
edge-cloud/
├── wrangler.jsonc            # observability、GALLERY_URL
├── src/index.ts              # 全応答のログ (event / kind / status / ms / colo)、転送例外 → 502
├── src/proxy.ts              # frame の Content-Length: 無し 411 / 不正 400 / 超過 413 (6a、経路確認後)
├── test/proxy.test.ts        # 411 / 400 / 413、ログが 1 件出て鍵・URL を含まないこと
└── README.md                 # 鍵の生成・ローテーション、計測手順、app を開いたままにしない注意
edge/tools/webcam_device.py   # --stats、--candidate-via-edge
docs/measurements/            # 計測結果 (新規ディレクトリ)
firmware/
├── sdkconfig.defaults        # CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y 等を明示
├── README.md                 # 「edge と繋ぐ」を URL 設定に更新 (移行手順)、quick tunnel の手順
├── main/CMakeLists.txt       # PHOTOBOOTH_FRAME_RGB565 の伝播 (6c)
├── tests/                    # edge_url の単体テスト (6b)。CI の firmware-tests
└── main/apps/app_photobooth/
    ├── config.h              # HELLO_TIMEOUT_MS、SHUTTER_CANDIDATE_TIMEOUT_MS、FRAME_FORMAT_JPEG
    ├── config_local.example.h# EDGE_BASE_URL
    ├── net/edge_config.h     # kEdgeBaseUrl、旧 EDGE_HOST の #error
    ├── net/edge_client.h     # Diagnostics::edge_url、LinkState::Starting
    ├── net/edge_url.{h,cpp}  # URL の分解 (純粋関数) (6b)
    ├── net/http_edge_client.cpp # transport の選択、crt_bundle、時刻の確認と SNTP 待ち、タイムアウト表、エラー分類、Starting (6b)、JPEG 符号化と所有権 (6c)
    ├── net/null_edge.h       # Diagnostics の変更に追従
    ├── flow/flow.cpp         # LinkState 3 値の変換 (:478)
    ├── view/view.h, view.cpp # IdleLink 3 値 (:36, :45)
    └── view/strings.h        # 文言
```

## 5. 試験表

| # | 条件 | 期待 | サブステップ |
| --- | --- | --- | --- |
| 1 | 鍵なし / 鍵違い / `/v1/` 以外 | 401 / 401 / 404。コンテナが起動しない (ダッシュボード)。ログが各 1 件 | 6a |
| 2 | `GALLERY_URL` 空 | 503 `misconfigured`、起動しない | 6a |
| 3 | cold start で hello (`curl --max-time 8` で切る) | 最初はタイムアウトか 5xx、再試行で 200。**クライアントが切断してもコンテナの起動が続く** (続かなければ §3.2 の `503 starting` 方式に変える) | 6a |
| 4 | frame の Content-Length: 正常 / 無し / 非数値 / 2 MiB 超 | edge に届く (411 を返さない) / Worker が 411 / 400 / 413。後ろ 3 つはコンテナを起こさない | 6a |
| 5 | candidate の応答形式 | chunked か Content-Length か記録 | 6a |
| 6 | webcam で RGB565 / JPEG 各 10 分 | 統計 (件数つき) を記録 | 6a |
| 7 | 撮影中にコンテナを止める: (a) `wrangler deploy` で rollout、(b) 疑似デバイスを一時停止して 5 分待つ (heartbeat を止める) → 再開 | device が `unknown_session` / 接続断で ERROR → 撮り直しで復帰 | 6a (webcam) / 6d (K151) |
| 8 | Worker の構造化ログ | 早期 401 / 411 / 413、転送成功、転送例外の全部で 1 件出る。鍵・URL・画像を含まない | 6a |
| 9 | K151 を `https://` (quick tunnel) に接続、RGB565 | hello 200、frame 200 (Content-Length が FastAPI まで届く)、candidate が読める、診断に URL | 6b |
| 10 | K151 を `http://IP:port` に接続 | 従来どおり動く | 6b |
| 11 | 旧 `config_local.h` (`EDGE_HOST`) のままビルド | `#error` で止まり、移行手順を示す | 6b |
| 12 | `edge_url` の単体テスト | 大小文字、既定 port、明示 port、末尾 `/`、path / query / fragment / userinfo、長い host、port 範囲外、IPv6、https + IP の各ケース | 6b |
| 13 | 証明書が検証できない相手 (`expired.badssl.com`、`self-signed.badssl.com`) | 診断に「証明書エラー」。接続は成立しない | 6b |
| 14 | 時刻未同期 (SNTP を通さない Wi-Fi、RTC を 1970 年にして起動) | 「時刻未同期」が出る。同期後に復帰 | 6b |
| 15 | DNS 失敗 (存在しないホスト名) / TCP 失敗 (閉じたポート) | 「DNS失敗」/「接続できません」 | 6b |
| 16 | Wi-Fi 断 → 復帰 | 「接続できません」→「接続中」 | 6b |
| 17 | 相手側の切断: (a) PC の edge プロセスを再起動、(b) `cloudflared` を止めて再起動 | 1 回の再送で復帰 (a)。(b) は URL が変わるので旧 URL が確実に失敗する | 6b |
| 18 | cold start の模擬: edge をモデル読み込み中にする (`cloudflared` は上げたまま edge を再起動) | 「準備中…」→「接続中」 | 6b |
| 19 | アプリを閉じる (通信中) | 1 秒で画面が閉じる。再度開ける (タスク残存中は「前の通信が終わっていません」) | 6b |
| 20 | タイムアウト境界: hello 8 秒、frame 3 秒、SHUTTER 1.5 秒 (edge 側で応答を遅らせるデバッグ設定) | それぞれの文言と状態になる | 6b |
| 21 | JPEG で 10 分撮影を回す | 失敗率 < 2 %、音切れ・プレビューの fps 低下が無い。最大 JPEG サイズを記録 | 6c |
| 22 | 符号化失敗の注入 (出力確保を失敗させるデバッグ定義) | フレームが破棄され、統計に出る。アプリは落ちない | 6c |
| 23 | `-DPHOTOBOOTH_FRAME_RGB565=1` | RGB565 で従来どおり | 6c |
| 24 | 統合: Worker 経由で cold start からの一周、QR をモバイル回線で開く | 合格基準を満たす | 6d |

## 6. 受け入れチェック

- [x] 6a: `wrangler deploy` で Worker が立ち、試験 1〜8 を記録した。`docs/measurements/step6-<date>.md` がある。→ [step6-2026-10-10.md](../measurements/step6-2026-10-10.md)。試験 3 で切断後も起動が続いたので `503 starting` は不要。試験 7a は `wrangler deploy` で rollout が起きず、Containers API で同じイメージの rollout を作って確かめた
- [ ] 6b: `idf.py build` (`config_local.h` あり http / あり https / なし) が通る。`firmware-tests` に `edge_url` のテストが入り CI が通る。試験 9〜20。
- [ ] 6c: 試験 21〜23。`logStats()` に JPEG の統計が出る。
- [ ] 6d: 試験 24。決めた値 (`config.h`、`wrangler.jsonc`) と根拠を計測記録に書いた。
- [ ] 各サブステップで codex レビューを通し、指摘を反映した。
- [ ] Worker とコンテナのログに鍵・画像・写真 URL が出ない。

## 7. 設定と秘密

- `edge-cloud/wrangler.jsonc` の `vars`: `DEVICE_ID = "stackchan-01"`、`GALLERY_URL = "https://stackchan-gallery.na2kera.workers.dev"`。
- Secret: `EDGE_DEVICE_KEY` (device と共通、`openssl rand -hex 32`)、`GALLERY_KEY` (`gallery/.dev.vars` と同じ値)。
- K151: `config_local.h` (gitignore) の `EDGE_BASE_URL` / `DEVICE_ID` / `EDGE_SHARED_KEY`。サブエージェントには触らせない (ユーザーが書く)。
- ローテーション: 新しい鍵を `wrangler secret put` → device の app を閉じる → 5 分待つ → `config_local.h` を更新して書き込み。
- デプロイの戻し: `wrangler rollback` (Worker)。コンテナのイメージも前のバージョンに戻る。環境変数の反映は上と同じく停止待ちが要る。

## 8. プライバシー

step5 §2「プライバシー」のとおり。6c で JPEG になっても経路と保持範囲は変わらない。計測記録には画像を含めない。
