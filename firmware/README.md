
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

（フォークで追加）自作アプリのハードに依存しないロジックも同じ手順でテストする。約束事は
`.claude/skills/firmware-testable-design/SKILL.md`、CI では `.github/workflows/ci.yml` の `firmware-tests` が走る。

| テスト | 対象 |
| --- | --- |
| `motion_math_test` | 純正 `main/stackchan/motion/motion_math` |
| `roulette_slot_test` | `app_roulette/game/slot` (スロットの状態) |
| `light_pattern_test` | `app_roulette/hw/light_pattern` (頭部 LED の色) |
| `wav_test` | `shared/wav` (埋め込み WAV のヘッダ) |
| `jpeg_info_test` | `app_photobooth/view/jpeg_info` (JPEG のヘッダ) |
| `edge_parse_test` | `app_photobooth/net/edge_parse` (edge の応答の解釈) |
| `head_logic_test` | `app_photobooth/hw/head_logic` (首の応答監視。サーボは偽物) |
| `time_format_test` | `app_photobooth/flow/time_format` (写真の期限の表示) |
| `edge_url_test` | `app_photobooth/net/edge_url` (`EDGE_BASE_URL` の分解) |
| `link_logic_test` | `app_photobooth/net/link_logic` (接続の失敗の分類・タイムアウト表・接続状態・定期 hello) |
| `frame_stats_test` | `app_photobooth/net/frame_stats` (frame の形式の選択・符号化失敗の注入・送信統計の集計) |
| `edge_config_test_*` / `edge_config_rejects_*` | `app_photobooth/net/edge_config.h` (`config_local.h` の書き方の受け付けと旧形式の検出。`tests/edge_config_fixtures/`) |

- `edge_parse_test` は ArduinoJson を使う。`components/ArduinoJson` (`fetch_repos.py` が取る) があればそれを、
  無ければ CMake の `FetchContent` で `repos.json` と同じ v7.4.2 を取る (初回はネットワークが要る)。
- macOS でリンクに失敗する (MacOSX27 SDK の `.tbd` を読めない) ときは、
  `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk` を付けて `cmake -S tests ...` からやり直す。
- ビルド先はリポジトリの外 (例: `/tmp/stackchan-tests`) にしてよい。

### Flash

```bash
idf.py flash
```

## Photobooth fork (na2kera/StackChan `photobooth` ブランチ)

このフォークは純正ファームに写真撮影アプリ `main/apps/app_photobooth/` とスロット `main/apps/app_roulette/` を足したもの。
設計は親リポジトリ (photobooth) の `docs/design/fw-app-step1.md` と `docs/design/fw-app-step2.md` (edge 接続)、
`docs/design/app-roulette.md` (スロットと共通部品)。
upstream との差分は次だけにしている。

| 変更 | 場所 |
| --- | --- |
| アプリ本体 (ランチャーの最後に「Photobooth」「Slot」の順で並ぶ) | `main/apps/app_photobooth/`, `main/apps/app_roulette/` |
| 2 つのアプリで使う部品 (WAV 再生 `shared::hw::Audio`・頭部/画面タップ `shared::hw::Input`・素材の生成スクリプト) | `main/apps/shared/` (純正の `main/apps/common/` には手を入れない) |
| アプリの登録 | `main/apps/apps.h` の include 各 1 行、`main/main.cpp` の `installApp` 各 1 行 (AppPhotobooth → AppRoulette の順で最後に足す) |
| セリフ WAV の埋め込み | `main/CMakeLists.txt` の `PHOTOBOOTH_VOICES` / `ROULETTE_VOICES` (`EMBED_FILES`。ファイル名がシンボル名になるので、スロットの WAV は `rl_` 始まり) |
| edge 通信なしでビルドするスイッチ | `main/CMakeLists.txt` の `PHOTOBOOTH_NO_EDGE` (`idf.py -DPHOTOBOOTH_NO_EDGE=1 build`) |
| frame を RGB565 のまま送るスイッチ・符号化失敗の注入 (試験用) | `main/CMakeLists.txt` の `PHOTOBOOTH_FRAME_RGB565` / `PHOTOBOOTH_JPEG_FAIL_EVERY` |
| 起動時の自動ファーム更新を止める | `patches/xiaozhi-esp32.patch` (`xiaozhi-esp32/main/application.cc` の `CheckNewVersion()`) |

### 自動更新を止めている理由と範囲

純正は AI エージェント起動時の版チェック (`Application::CheckNewVersion()`) で新しい版があると
自動で書き換えて再起動するため、このビルド (と app_photobooth) が純正に戻ってしまう。
パッチでは「新版があっても `UpgradeFirmware()` を呼ばずにログを出すだけ」にしている。
版チェック自体は活性化と MQTT / WebSocket 設定の取得を兼ねるので残している (AI エージェントはそのまま動く)。

- 手動更新は残している: SETUP の更新 (`Hal::updateFirmware`) や AI に頼む `self.upgrade_firmware` を実行すると、
  純正ファームに置き換わって app_photobooth は消える。戻すときはこのリポジトリから書き込み直す。
- `fetch_repos.py` は `xiaozhi-esp32` を clone したあと `patches/xiaozhi-esp32.patch` を当てる。
  パッチを変えるときは `xiaozhi-esp32/` で編集してから `git -C xiaozhi-esp32 diff > patches/xiaozhi-esp32.patch`。
  (`xiaozhi-esp32/` と `components/` は .gitignore 済みで、コミットしない)
- 既に `xiaozhi-esp32/` がある (前の版のパッチが当たっている) 環境では、`fetch_repos.py` が
  「cannot be applied cleanly, skipped」と出してパッチを当てない。その場合は
  `git -C xiaozhi-esp32 checkout -- .` で戻してから `python3 ./fetch_repos.py` を実行し直す。
  当たっているかは `grep -n "photobooth fork" xiaozhi-esp32/main/application.cc` で確認できる。

### edge と繋ぐ

アプリは `edge` (同じ LAN の PC、またはクラウド) に HTTP / HTTPS でフレームを送り、顔判定・首振り量・写真の保存を任せる
(通信契約は親リポジトリの `docs/protocol.md`)。接続先は `config_local.h` の `EDGE_BASE_URL` 1 つで決まる。Wi-Fi は純正の設定 (NVS) をそのまま使うので、
SSID / パスワードはコードに書かない。

1. **Wi-Fi**: 純正の SETUP で Wi-Fi を設定しておく (AI エージェントや App Center が繋がる状態)。2.4 GHz のみ。
   アプリを開くと、純正の App Center と同じく `GetHAL().startNetwork()` を呼んで接続を待つ
   (その間は「Wi-Fi接続中」の画面。繋がると待機画面へ進む)。アプリを閉じても Wi-Fi は切らない。
   - Wi-Fi が未設定 (NVS に SSID が無い) のときは `startNetwork()` を呼ばず、すぐ待機画面 (右下が「接続なし」) になる。
     診断画面には「未設定」と出る。SETUP で設定してからアプリを開き直す。
   - SSID は保存されているが繋がらない (圏外・パスワード違い) ときは**純正の動作**になる: 約 60 秒後に
     Wi-Fi 設定モード (AP) に入り、繋がるまで「Wi-Fi接続中」の画面から進まない。App Center を開いたときと同じ。
     抜けるには、その AP 経由で設定し直すか、本体をリセットする。
   - 一度繋がったあとに切れた場合の再接続は純正 (WifiManager) が自動で行う。診断画面の「再接続」は
     edge との HTTP 接続だけを作り直す (Wi-Fi には触らない)。
2. **設定ファイル**: `main/apps/app_photobooth/config_local.example.h` を `config_local.h` にコピーして書き換える
   (`config_local.h` は .gitignore 済み。コミットしない)。

   | 項目 | 値 |
   | --- | --- |
   | `EDGE_BASE_URL` | edge の URL。`scheme://host[:port][/]` (path・query・userinfo は書けない)。LAN の PC なら `"http://<PC の IP>:8765"` (IP で書く。Mac なら `ipconfig getifaddr en0`)、クラウドなら `"https://stackchan-edge.<account>.workers.dev"` (https はホスト名で書く。IP は不可) |
   | `DEVICE_ID` / `EDGE_SHARED_KEY` | edge の `config.toml` の `[auth] device_id` / `device_key` (または環境変数 `EDGE_DEVICE_KEY`) と同じ値 |

   **旧形式からの移行**: 以前の `EDGE_HOST` / `EDGE_PORT` は使えない (旧形式のままビルドすると
   `config_local.h: EDGE_HOST/EDGE_PORT は EDGE_BASE_URL に変わりました` でビルドが止まる)。
   `EDGE_HOST = "192.168.0.167"`、`EDGE_PORT = 8765` だったなら、2 行を消して次の 1 行に置き換える。

   ```cpp
   constexpr const char* EDGE_BASE_URL = "http://192.168.0.167:8765";  // http://<旧 EDGE_HOST>:<旧 EDGE_PORT>
   ```

   `config_local.h` の値は `namespace photobooth::config { ... }` の中の `constexpr`、グローバル名前空間の
   `constexpr`、`#define` のどれで書いてもよい。書式が不正だと待機画面が「接続なし」、診断画面に
   「接続先の書式が不正」と出る。

   https のときは証明書を常に検証する (ESP-IDF の証明書バンドル。`sdkconfig.defaults` の
   `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`)。有効期間も検証する (`CONFIG_MBEDTLS_HAVE_TIME_DATE=y`)。
   システム時刻が 2025 年より前なら、最初の `hello` の前に SNTP の同期を最大 5 秒待つ。
   - `CONFIG_MBEDTLS_HAVE_TIME_DATE` はファーム全体の設定なので、純正の TLS (AI エージェント、手動 OTA) も
     本体の時刻が正しくないと証明書の検査で失敗する (RTC が戻っていれば起動直後でも動く。SNTP の同期後は動く)。
   - 手動 OTA の埋め込み証明書 (`main/hal/utils/ota/ota.c`) は 2027-01-20 に期限が切れる。以後は手動 OTA が
     証明書エラーになるので、証明書を更新する。

3. **edge を LAN 向けに起動する**: `127.0.0.1` で listen すると K151 から届かない。

   ```bash
   cd edge
   EDGE_DEVICE_KEY=<config_local.h と同じ鍵> uv run edge --host 0.0.0.0
   ```

   macOS のファイアウォールが有効なら、Python への受信を許可する。frame は既定で JPEG (下の「frame の形式」)。
   RGB565 で送るビルドのときのバイト順は edge 設定の `rgb565_byte_order = "little"`
   (アプリはカメラ層の RGB565 リトルエンディアンをそのまま送る)。
4. `idf.py build` して書き込み、ランチャーから Photobooth を開く。待機画面の右下が「接続中」になれば繋がっている。
   「準備中」は接続はできたが edge の応答待ち (cloud の cold start など。5 秒ごとの `hello` で再試行する)。
   「接続なし」のまま画面をタッチすると診断画面が開き、Wi-Fi の状態 / IP / 接続先 `scheme://host:port` / 応答 /
   最後のエラーが読める。「再接続」で edge との接続を作り直してすぐ `hello` を送り、「判定なしで撮影」で edge なしの
   撮影 (保存はできない) を試せる。頭をタッチすると待機画面に戻る。
   - PC の IP は DHCP で変わることがある。診断画面のエラーが「接続できません」のときは `ipconfig getifaddr en0` と
     診断画面の「接続先」の行を見比べ、違っていたら `config_local.h` を直して書き込み直す。
   - 診断画面の「エラー」: 「DNS失敗」(ホスト名を引けない)、「接続できません」(TCP で繋がらない)、
     「証明書エラー」(証明書を検証できない)、「時刻未同期」(本体の時刻が不正で証明書の期限を検査できない)、
     「TLS接続失敗」(その他の TLS の失敗)、「準備中 (応答待ち)」(接続後に `hello` の応答が 8 秒以内に来ない)。
   - 「認証エラー(IDか鍵が違う)」は `DEVICE_ID` / `EDGE_SHARED_KEY` と edge 側の不一致 (HTTP 401)。
   - シリアルログの `PB-Edge: send N frames (jpeg) ... (x.x fps), attempts N, frame failures N (send N, encode N),
     rtt avg ...` (5 秒ごと) で送信 fps と往復時間、frame の失敗の内訳が分かる。JPEG のときは
     `PB-Edge: jpeg: encoded N, jpeg_encode_ms avg .. max .., size avg .. max .. B, failed N` (符号化の所要と大きさ) と
     `PB-Edge: heap: internal free .. min .. largest .., psram free .. largest ..` (内部 RAM の最小空き・最大連続領域)
     も出る。
     5 秒ごとに `PB-Edge: hello N (ok N, starting N) rtt avg ... max ... ms, link ..., stack free ...` と
     `PB-Edge: connections: new N reused M` も出る (https で new ばかりなら毎回 TLS ハンドシェイクしている)。
     定期 `hello` は判定つきの撮影の開始から写真の準備完了 (QR 表示) までは送らない (その間の接続状態は
     frame などの結果で決まる)。QR 表示中は送る。
     鍵・URL・トークン・画像はログに出さない。

**HTTPS を quick tunnel で試す** (クラウドの edge を立てずに、PC の edge を HTTPS で公開する): Cloudflare Tunnel の
quick tunnel は無料・アカウント不要で、`*.trycloudflare.com` の正規の証明書が付く。

```bash
brew install cloudflared
cd edge && EDGE_DEVICE_KEY=<config_local.h と同じ鍵> uv run edge          # 127.0.0.1:8765 のままでよい
cloudflared tunnel --url http://localhost:8765                             # 別のターミナルで
```

`cloudflared` が表示する `https://<ランダム>.trycloudflare.com` を `EDGE_BASE_URL` に書いて書き込む。
URL は起動ごとに変わり、SLA も無いので開発の確認専用 (本番の設定値には使わない)。`~/.cloudflared/config.yaml`
があると quick tunnel は起動しない。確かめられるのは端末 → Cloudflare の TLS (証明書・DNS・SNI) と持続接続までで、
クラウドの edge (Worker・コンテナ・cold start) の挙動は確かめられない。

**frame の形式**: 既定では net タスクが frame を JPEG (品質 80、QVGA で数十 KB) にして `X-Format: jpeg` で送る
(`docs/design/step6-cloud-device.md` §3.3)。`idf.py -DPHOTOBOOTH_FRAME_RGB565=1 build` でビルドすると 6b までと同じ
RGB565 (153,600 バイト、`X-Format: rgb565`) で送る。CMake のキャッシュに残るので、戻すときは
`idf.py -DPHOTOBOOTH_FRAME_RGB565=0 build`。符号化に失敗したフレームは捨てて (RGB565 では送らない) 統計に数える。
試験用に `idf.py -DPHOTOBOOTH_JPEG_FAIL_EVERY=N build` で N 回に 1 回符号化を失敗扱いにできる (戻すときは `=0`)。

**edge なしでビルドする**: `config_local.h` が無い、または `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` でビルドすると、
Wi-Fi に繋がず、ステップ1 と同じ単体動作 (QR は `config.h` の固定 URL) になる。`-DPHOTOBOOTH_NO_EDGE` は
CMake のキャッシュに残るので、戻すときは `idf.py -DPHOTOBOOTH_NO_EDGE=0 build`。

**今後の提案 (未実装)**: `EDGE_BASE_URL` の host を `.local` 名 (mDNS) で書けるようにすれば PC の IP が変わっても
書き込み直さずに済む。純正ファームには mDNS が入っていないので、`espressif/mdns` (component manager) の
追加が要る。依存を増やす変更なので、入れるかどうかは別途決める。

### 素材の作り直し

| 素材 | コマンド | 依存 |
| --- | --- | --- |
| セリフ (`assets/voice/*.wav`, 24 kHz / mono / 16-bit) | `main/apps/app_photobooth/tools/make_voice.sh` | macOS `say` (Kyoko), ffmpeg |
| 日本語フォント (`assets/pb_font_*.c`) | `main/apps/app_photobooth/tools/gen_font.sh` | python3, npx (lv_font_conv 1.5.3)。`idf.py build` 済み (managed_components が必要) |
| ランチャーのアイコン (`assets/icon_photobooth.c`) | `python3 main/apps/app_photobooth/tools/make_icon.py` | Pillow |
| スロットの発話 (`assets/voice/rl_start.wav`, `rl_reach.wav`) | `main/apps/app_roulette/tools/make_voice.sh` | macOS `say` (Kyoko), ffmpeg |
| スロットの日本語フォント (`assets/rl_font_jp_20.c`) | `main/apps/app_roulette/tools/gen_font.sh` | photobooth と同じ |
| スロットのリール (`assets/rl_reel.c`, 72x288 RGB565) | `python3 main/apps/app_roulette/tools/make_reel.py` | rsvg-convert (librsvg), magick (ImageMagick) |
| スロットのアイコン (`assets/icon_roulette.c`) | `python3 main/apps/app_roulette/tools/make_icon.py` | Pillow (無ければ `uv run --with pillow python3 ...`) |

各アプリの `make_voice.sh` / `gen_font.sh` は `main/apps/shared/tools/` の同名スクリプトを呼ぶだけ
(`shared/tools/gen_font.sh <strings.h> <出力.c> <px> [--ascii]`、`shared/tools/make_voice.sh <出力ディレクトリ> <名前> <文言>...`)。

画面の文言は各アプリの `view/strings.h` にまとめてある。文言を変えたら `gen_font.sh` で
フォントを作り直す (純正同梱の `font_puhui_basic_20_4` には一部のかな・漢字が無いため、
同じ PuHuiTi 系の `puhui-common.ttf` から使う文字だけを切り出している)。

### 書き込みと復旧

```bash
idf.py build
idf.py flash monitor
```

純正に戻すときは M5Burner で StackChan の純正ファームを書き込む (従来どおり)。
