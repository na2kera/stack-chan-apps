
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

### edge (PC) と繋ぐ

アプリは同じ LAN の PC で動く `edge` に HTTP でフレームを送り、顔判定・首振り量・写真の保存を任せる
(通信契約は親リポジトリの `docs/protocol.md`)。Wi-Fi は純正の設定 (NVS) をそのまま使うので、
SSID / パスワードはコードに書かない。

1. **Wi-Fi**: 純正の SETUP で Wi-Fi を設定しておく (AI エージェントや App Center が繋がる状態)。2.4 GHz のみ。
   アプリを開くと、純正の App Center と同じく `GetHAL().startNetwork()` を呼んで接続を待つ
   (その間は「Wi-Fi接続中」の画面。繋がると待機画面へ進む)。アプリを閉じても Wi-Fi は切らない。
   - Wi-Fi が未設定 (NVS に SSID が無い) のときは `startNetwork()` を呼ばず、すぐ待機画面 (「PC未接続」) になる。
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
   | `EDGE_HOST` | edge を動かす PC の LAN アドレス (IP で書く)。Mac なら `ipconfig getifaddr en0` |
   | `EDGE_PORT` | edge の待ち受けポート (既定 8765) |
   | `DEVICE_ID` / `EDGE_SHARED_KEY` | edge の `config.toml` の `[auth] device_id` / `device_key` (または環境変数 `EDGE_DEVICE_KEY`) と同じ値 |

3. **edge を LAN 向けに起動する**: `127.0.0.1` で listen すると K151 から届かない。

   ```bash
   cd edge
   EDGE_DEVICE_KEY=<config_local.h と同じ鍵> uv run edge --host 0.0.0.0
   ```

   macOS のファイアウォールが有効なら、Python への受信を許可する。RGB565 のバイト順は edge 設定の
   `rgb565_byte_order = "little"` (アプリはカメラ層の RGB565 リトルエンディアンをそのまま送る)。
4. `idf.py build` して書き込み、ランチャーから Photobooth を開く。待機画面の右下が「PC接続中」になれば繋がっている。
   「PC未接続」のまま画面をタッチすると診断画面が開き、Wi-Fi の状態 / IP / 接続先 `host:port` / PC の応答 /
   最後のエラーが読める。「再接続」で edge との接続を作り直してすぐ `hello` を送り、「判定なしで撮影」で edge なしの
   撮影 (保存はできない) を試せる。頭をタッチすると待機画面に戻る。
   - PC の IP は DHCP で変わることがある。「PCに接続できません」のときは `ipconfig getifaddr en0` と
     診断画面の「PC」の行を見比べ、違っていたら `config_local.h` を直して書き込み直す。
   - 「認証エラー(IDか鍵が違う)」は `DEVICE_ID` / `EDGE_SHARED_KEY` と edge 側の不一致 (HTTP 401)。
   - シリアルログの `PB-Edge: send N frames ... (x.x fps), rtt avg ...` (5 秒ごと) で送信 fps と往復時間が分かる。
     鍵・URL・トークン・画像はログに出さない。

**edge なしでビルドする**: `config_local.h` が無い、または `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` でビルドすると、
Wi-Fi に繋がず、ステップ1 と同じ単体動作 (QR は `config.h` の固定 URL) になる。`-DPHOTOBOOTH_NO_EDGE` は
CMake のキャッシュに残るので、戻すときは `idf.py -DPHOTOBOOTH_NO_EDGE=0 build`。

**今後の提案 (未実装)**: `EDGE_HOST` を `.local` 名 (mDNS) で書けるようにすれば PC の IP が変わっても
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
