# 純正ファーム内アプリ ステップ1 設計: app_photobooth を純正ファームに追加して単体フローを通す

方針転換（2026-10-01）: 独立ファーム（`device/`）ではなく、**M5Stack 純正ファームをフォークしてアプリとして追加**する。純正のホーム画面（ランチャーの顔）・AI エージェント・既存アプリはそのまま残る。
`device/` は参照用に残し、以後更新しない。edge（`edge/`）と通信契約（`docs/protocol.md`）はそのまま使う。

## 1. ゴールと非ゴール

ゴール（独立ファーム版ステップ1 と同じフローを純正ファーム内で）:

- ランチャーのアプリ一覧に「Photobooth」が並び、開くと待機画面（「写真を撮りたい、と言ってね」「タッチで開始」）になる。
- 画面タッチまたは頭部タッチで ANNOUNCE → COMPOSE → CAPTURE → REVIEW → UPLOADING → QR（写真 QR と X 投稿 QR を 1 画面に並べる）と一周し、「終了」でアプリを閉じてランチャーの顔に戻る（変更: 旧 PHOTO_QR → X_QR の 2 画面を統合。`ui-review-two-qr.md`）。
- カメラプレビュー、首振り（小さな探索）、セリフ再生、固定 URL の QR が動く。
- 純正の自動 OTA でこのビルドが上書きされないようにする。
- 純正機能（AI エージェント、アバター、ダンス、設定、App Center）が壊れていない。

非ゴール（次のステップ）:

- edge 接続（fw-app ステップ2 = 独立ファーム版 2b の移植）。音声起動（ステップ3）。gallery（ステップ4）。

## 2. リポジトリ構成

```
stack-chan-apps/
├── firmware/             # m5stack/StackChan の firmware/ を git subtree (squash) で取り込んだ ESP-IDF プロジェクト。
│   └── main/apps/app_photobooth/   # ここに足す (将来 app_roulette も)
├── device/               # 旧・独立ファーム (参照用、更新しない)
├── edge/                 # 変更なし
└── docs/
```

- 2026-10-01 にサブモジュール方式から subtree 方式に変更した（1 リポジトリで完結させ、PR にアプリのコードが載るようにするため）。
- upstream の追従: `git clone https://github.com/m5stack/StackChan /tmp/sc && git -C /tmp/sc subtree split --prefix=firmware -b firmware-only` で firmware/ だけの履歴を作り、`git subtree pull --prefix=firmware /tmp/sc firmware-only --squash` で取り込む。衝突するのは下記の数ファイルだけ。
- 純正に入れる変更は「`main/apps/app_photobooth/` の追加」「`main.cpp` の `installApp` 1 行と `apps/apps.h` の include 1 行」「OTA 自動更新の無効化（`patches/xiaozhi-esp32.patch`）」「`main/CMakeLists.txt` への音声ファイル埋め込み」「`firmware/README.md` への追記」に限定し、upstream の追従を楽にする。
- `installApp` は `AppSetup` の後（最後）に足す。各アプリの `requestWarmReboot(index)` が install 順の番号を直書きしているため、途中に挟むと再起動後に別のアプリへスクロールしてしまう。

## 3. 技術選定（固定値）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| ESP-IDF | v5.5.4（`~/esp/esp-idf-v5.5.4`） | 純正 `firmware/README.md` 指定 |
| ターゲット | esp32s3 | |
| アプリ基盤 | Mooncake v2.3.3 `AppAbility`（`app_template` 準拠） | 純正と同じ |
| UI | LVGL 9.4 + smooth_ui_toolkit lvgl_cpp | 純正と同じ。`CONFIG_LV_USE_QRCODE=y` 済みなので QR は `lv_qrcode` |
| カメラ | `hal_bridge::board_get_camera()`（esp_video / V4L2 の `StackChanCamera`）。`StreamCaptures()` → `GetFrameData/Size/Width/Height/Format()` | 純正のビデオ通話（`hal_ws_avatar.cpp`）と同じ経路 |
| 画像 | JPEG 化は `jpg/image_to_jpeg.h`、JPEG → LVGL は `utils/jpeg_to_image/jpeg_decoder.h` | 純正に同梱 |
| 音声出力 | `Board::GetInstance().GetAudioCodec()` の `EnableOutput(true)` + `OutputData(std::vector<int16_t>&)`（`main/hal/audio.cpp` のマイクテストと同じ使い方） | 純正のコーデック経路。出力は `AUDIO_OUTPUT_SAMPLE_RATE` = 24 kHz / mono（`main/hal/board/config.h`）なので WAV は 24 kHz / mono / 16-bit |
| 首 | `GetStackChan()` に付いている `motion::Motion`（`moveWithSpeed(yaw, pitch, speed)`、単位 1/10 度、`goHome`、`stop`、`setTorqueEnabled`） | 純正と同じ。可動域・neutral・ステップ上限の考え方は `docs/design/step1-device.md` §5 head を踏襲 |
| 頭部タッチ | `GetHAL().onHeadPetGesture`（Press / Release / Swipe） | |
| 画面タッチ | LVGL のボタン／全画面クリック領域 | |
| 時計 | `GetHAL().millis()` | |
| ログ | `mclog::tagInfo/Warn/Error` | 純正と同じ |

## 4. アプリ構成（`firmware/main/apps/app_photobooth/`）

```
app_photobooth/
├── app_photobooth.h/.cpp   # AppAbility。onOpen で初期化、onRunning で状態機械を回す、onClose で後始末
├── flow/
│   ├── state.h             # enum State (IDLE, ANNOUNCE, COMPOSE, CAPTURE, SHUTTER, REVIEW, UPLOADING, QR, ERROR, DIAG)
│   ├── flow.h/.cpp         # 状態機械。docs/design/step1-device.md §4 と同じ遷移。HW は下の hw/ 経由
│   └── session.h           # UUID v4 / frame_id / 単調時計 (device/src/app/session.h を移植)
├── hw/
│   ├── camera.h/.cpp       # board_get_camera() ラッパ。取り込みタスク (StreamCaptures → RGB565)、最新フレームの受け渡し、候補 1 枚の保持
│   ├── head.h/.cpp         # Motion ラッパ。クランプ・ステップ制限・neutral・応答監視 (device/src/hal/head.cpp を移植)
│   ├── audio.h/.cpp        # 埋め込み WAV → PCM 再生 (別タスクで OutputData)、isPlaying()
│   └── input.h/.cpp        # 頭部タッチ信号と LVGL のタップを Event に正規化
├── view/
│   ├── view.h/.cpp         # 画面ごとの LVGL 構築・更新。LvglLockGuard 必須
│   ├── widgets.h/.cpp      # タイトル帯、ボタン帯 (最大 2)、QR、プレビュー画像、カウント表示
│   └── strings.h           # 画面の文言をここに集約 (日本語フォントはこの文字だけから作る)
├── assets/
│   ├── pb_assets.h         # フォント・アイコン・WAV シンボルの宣言
│   ├── icon_photobooth.c   # ランチャー用アイコン (188x150 RGB565A8 の LVGL C 配列。assets パーティションは触らない)
│   ├── pb_font_jp_20.c     # 日本語 20px (ASCII + strings.h の文字)。lv_font_conv で生成
│   ├── pb_font_num_48.c    # 残り秒数用の数字 48px
│   └── voice/*.wav         # announce / captured / closer (CMake の EMBED_FILES で埋め込み)
├── tools/                  # make_voice.sh / gen_font.sh / make_icon.py (素材の再生成)
└── config.h                # 可動域・neutral・秒数・固定 URL など (device/include/config.example.h の該当分)
```

- `flow/` は LVGL・HAL を直接呼ばない（`hw/` と `view/` のインターフェース経由）。独立ファーム版の `app/app.cpp` の構造をそのまま移植する。
- LVGL の操作は必ず `LvglLockGuard` の中で行う（純正の `app_template` の注意書き）。
- `onRunning()` は Mooncake のループから呼ばれるので、ブロッキングしない。状態機械は 1 tick = 1 回の `update()`。

## 5. 独立ファーム版からの差分（純正ファームの都合）

| 項目 | 独立ファーム版 | 純正ファーム内アプリ版 |
| --- | --- | --- |
| IDLE | 自前の顔を描く | ランチャーの顔が IDLE 相当。アプリを開いた直後の画面は「待機」（文言は spec §4 の IDLE と同じ。ボタン帯に「終了」）。待機の「終了」と QR 画面の「終了」で `close()` してランチャーへ（QR 画面の「終了」は IDLE に戻らない） |
| 画面描画 | M5GFX 直描き | LVGL。プレビューは `lv_image` に RGB565 バッファを張り替える（または `lv_canvas`）。カウントなどは上に重ねたラベルを更新 |
| 日本語フォント | lgfxJapanGothic | 確認結果: 純正が組み込む `font_puhui_basic_20_4` には一部のかな・漢字（あ と ね 撮 写 など 40 字）が無い。同じ PuHuiTi 系の `puhui-common.ttf`（xiaozhi-fonts 同梱）から `view/strings.h` の文字 + ASCII だけを `lv_font_conv` で切り出した `pb_font_jp_20`（20px / 4bpp）と、残り秒数用の `pb_font_num_48` を `assets/` に置く。漢字の字形は中国語系 |
| 音声 | M5.Speaker.playWav | コーデックの PCM 出力（24 kHz / mono）。WAV はビルド時に埋め込み、再生タスクで 20ms ずつ `OutputData`。音量は純正の設定値（SETUP）に従う |
| マイク | M5.Mic | 今回は使わない。AI エージェント（xiaozhi）がコーデック入力を持つのはそのアプリを開いている間だけなので競合しない |
| 首 | StackChan-BSP Motion | 純正 Motion（API はほぼ同じ。ただし `update()` を呼ばないと動かないので、アプリが毎 tick 呼ぶ）。純正は止まるとトルクを自動で抜くので、撮影中だけ自動解放を止めて姿勢を保つ。アプリを閉じるときに neutral に戻す指示を出し、自動解放を純正の既定（有効）に戻す |
| 設定 | config.h（.gitignore） | `app_photobooth/config.h` をコミット（ステップ1は秘密が無い）。鍵はステップ2 で別ファイルに分けて .gitignore する。純正の Wi-Fi 設定（NVS）を使えるので SSID/パスワードの直書きは不要になる見込み |
| 自動更新 | なし | `application.cc` の `CheckNewVersion()` で新版があっても書き換えないようにした（`patches/xiaozhi-esp32.patch`）。版チェック自体は活性化と MQTT/WebSocket 設定の取得を兼ねるので残す。SETUP からの手動更新は使える（実行すると純正に戻る）。`firmware/README.md` に明記 |
| カメラ | esp_camera を自前で init / deinit | カメラは純正のボード初期化が持つ。プレビューは取り込みタスクで `StreamCaptures()` → YUYV を esp_imgfx で RGB565 に変換。ERROR の「再試行」はドライバを作り直さず取り込みタスクだけ作り直す |
| 復旧 | M5Burner | 同じ（純正ファーム＝このビルドの元なので、純正に戻すのも M5Burner） |

## 6. 実装順（このステップの中）

1. **環境**: ESP-IDF v5.5.4 を入れ、`fetch_repos.py` → `idf.py set-target esp32s3` → `idf.py build` で**純正ファームをそのままビルド**し、K151 に書き込んでホーム画面・AI エージェントが動くことを確認する。ここで止まったら環境の問題。
2. **雛形**: `app_template` を複製して `app_photobooth` を作り、`main.cpp` に `installApp`。ランチャーにアイコンと名前が出て、開くと「待機」画面、QUIT で戻ることを確認。
3. **hw/**: カメラ（プレビュー表示まで）、首（neutral と探索）、音声（announce.wav が鳴る）、入力（頭部・画面タップ）を 1 つずつ確認する小さな画面を作ってもよい（PR には残さない）。
4. **flow/ と view/**: 状態機械と画面を移植し、一周を通す。
5. **OTA 無効化**と README（ビルド・書き込み・復旧）。

## 7. 受け入れチェック（PR テンプレートに転記）

- [ ] 純正ビルド（アプリ追加前）を書き込んでホーム画面・設定・AI エージェントが動く。
- [ ] ランチャーに「Photobooth」が出て、開くと待機画面、「終了」でランチャーの顔に戻る。
- [ ] 画面タッチ・頭部タッチで開始し、セリフが鳴り終わってから COMPOSE。
- [ ] COMPOSE で首が左右に小さく動いて正面に戻る。可動域は config の範囲内。
- [ ] CAPTURE のプレビューが動き、残り秒数が減り、10 秒で REVIEW。
- [ ] REVIEW の「撮り直す」／「次へ」（保存）、QR 画面の 2 つの QR（左: 写真、右: X 投稿）がスマホで読める。
- [ ] アプリを閉じたあと、純正のアバター・ダンス・AI エージェントが正常（カメラ・音声・サーボの後始末ができている）。
- [ ] 自動 OTA が走らない（AI エージェント起動時に更新チェックが出ない）。
- [ ] 日本語の文言が欠けずに表示される。

## 7.5 レビュー round 1 で決めたこと

- カメラ・音声のタスクが 1 秒以内に止まらない場合は、共有状態（`CameraWorker` / `AudioWorker`、shared_ptr）ごとタスクを切り離し、後始末はタスク側に任せる。切り離したタスクが生きている間は `begin()` / `restart()` を断り、ERROR「カメラを再起動できません」（音声は無音で続行）にする。
- 候補にするフレームは `captured_ms > Head::lastMotionMs() + HEAD_SETTLE_MS`（既定 150 ms）を満たすものだけ。
- プレビュー用バッファが確保できなければ ERROR「メモリ不足」から始める。
- 入力は FreeRTOS キュー（待ち時間 0）で LVGL タスクをブロックしない。

## 8. 実機で確認して反映する値

- ~~コーデックの出力サンプルレート（WAV の変換先）。~~ → コードで確定: 24 kHz（`main/hal/board/config.h`）。実機では音の高さ・速さが正しいかだけ確認する。
- プレビューの色（`SetSwapBytes`）と向き（`SetHMirror` / `SetVFlip`）。
- 首の neutral（純正の home と同じで良いか）。
