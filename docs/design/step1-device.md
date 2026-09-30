# ステップ1 設計: device 単体で撮影フローを通す

対象: `docs/spec.md` §11 の実装順 1。
「独立ファームの起動、タッチ開始、カメラプレビュー、首制御、セリフ、固定 QR」を K151 実機で動かす。
edge / gallery はまだ存在しないので、状態機械は完成形の形で作りつつ、判定と配布の部分だけスタブにする。

## 1. ゴールと非ゴール

ゴール:

- 電源投入で IDLE 画面（顔 + 「写真を撮りたい、と言ってね」）が出る。
- 画面タッチまたは頭部タッチで撮影が始まり、ANNOUNCE → COMPOSE → CAPTURE → REVIEW → PHOTO_QR → X_QR → IDLE を一周できる。
- CAPTURE 中はカメラプレビューと残り秒数が出る。COMPOSE 中に首が小角度で動いて正面に戻る。
- ANNOUNCE でセリフ WAV が再生され、再生完了まで次に進まない。
- PHOTO_QR / X_QR で設定ファイルの固定 URL を QR 表示する。実機のスマホカメラで読める大きさにする。
- 起動〜一周までのログ（状態遷移・所要時間・サーボ角）がシリアルに出る。

非ゴール（後のステップ）:

- Wi-Fi、edge との通信、音声起動、顔判定、gallery アップロード。
- 顔の位置に合わせた首振り（COMPOSE の首振りは「動作確認用の小さな探索」だけ）。

## 2. 技術選定（固定値）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| ビルド | PlatformIO + pioarduino `55.03.312-1`（Arduino-ESP32 3.3.12 / IDF 5.5.5） | 公式 platformio/espressif32 は core 2.0.x で、StackChan-BSP と M5Unified カメラ例の要求（core ≥ 3.2.2）を満たさない |
| board | `m5stack-cores3`、`default_16MB.csv` | K151 本体は CoreS3（ESP32-S3, 16MB Flash, 8MB PSRAM） |
| M5Unified / M5GFX | `0.2.24` / `0.2.31` | カメラ例の要求 M5Unified ≥ 0.2.11。BSP main が 0.2.21+ の IOExpander API 前提 |
| StackChan-BSP | main `8d4d6fc3b7a6be379c6317c45a02a30bff8c492e` | タグ 1.1.0 は M5Unified 0.2.21+ とコンパイル不可。次のタグが出たらタグに戻す |
| カメラ | Arduino core 同梱 `esp_camera`（GC0308, RGB565, QVGA, PSRAM, fb_count=2） | 公式カメラ例と同じ |
| QR | M5GFX 組み込み `qrcode()` | 追加ライブラリ不要 |
| 日本語フォント | M5GFX 組み込み `fonts::lgfxJapanGothic_20`（見出しは 24） | 追加フォント不要 |
| 音声 | `board_build.embed_files` で `data/*.wav` を埋め込み、`M5.Speaker.playWav()` | ファイル差し替えだけで音声を変えられる（spec §2「差し替え可能」） |

上の値は `device/platformio.ini` と一致させる。

## 3. ソース構成

```
device/
├── platformio.ini
├── include/
│   ├── config.example.h     # 設定サンプル（コミット）
│   └── config.h             # 実体（.gitignore）
├── data/
│   ├── announce.wav         # 「写真を撮るよ！ いい顔をしてね」16kHz/mono/16bit
│   └── captured.wav         # 「撮れたよ」
├── tools/make_voice.sh      # 仮音声の再生成
└── src/
    ├── main.cpp             # setup/loop。各モジュールを組み立てて App に渡すだけ
    ├── app/
    │   ├── state.h          # enum class State と名前文字列
    │   ├── app.h / app.cpp  # 状態機械。入力イベントと時間で遷移する。描画とHWは呼ぶだけ
    │   └── session.h        # session_id(UUID)/frame_id/単調時計 を持つ構造体（ステップ2で edge に送る）
    ├── hal/
    │   ├── camera.h/.cpp    # GC0308 ラッパ。begin / grab() / release()
    │   ├── head.h/.cpp      # サーボラッパ。安全域クランプ・ステップ制限・neutral()
    │   ├── audio.h/.cpp     # WAV 再生・効果音・mic/speaker 排他切替
    │   └── input.h/.cpp     # 画面タッチ(M5.Touch)と頭部タッチ(TouchSensor)を Event に正規化
    ├── ui/
    │   ├── screens.h/.cpp   # 状態ごとの描画関数。M5.Display にだけ依存
    │   └── widgets.h/.cpp   # ボタン矩形、QR、カウント表示などの部品
    └── edge/
        ├── edge_client.h    # 抽象インターフェース（ステップ2で実装が入る）
        └── null_edge.h      # 常に offline を返すスタブ
```

依存方向は `main → app → {hal, ui, edge}`。`ui` と `hal` は互いを知らない。`app` は `M5StackChan` を直接触らず、hal を経由する（テストと差し替えのため）。

## 4. 状態機械

`docs/spec.md` §4 の表をそのまま `State` にする。ステップ1で edge が無いぶん、次の置き換えをする。

| 状態 | ステップ1での振る舞い |
| --- | --- |
| IDLE | 顔と案内文を描画。画面タッチ（任意の場所）または頭部タッチ `wasClicked()` で ANNOUNCE へ。edge は offline なので画面下に小さく「PC未接続」を出す（spec §9） |
| ANNOUNCE | `audio.playAnnounce()`。`isPlaying()` が false になったら COMPOSE。マイクは止めた状態で入る |
| COMPOSE | プレビュー + 「みんな画面に入ってね」。首を neutral → 左に1ステップ → 右に1ステップ → neutral と動かす（各ステップ `HEAD_STEP_INTERVAL_MS` 間隔）。顔判定が無いので `COMPOSE_TIMEOUT_MS`（5秒）経過で CAPTURE |
| CAPTURE | 10秒カウント。プレビューに残り秒数を重ねる。首は動かさない。edge が offline なので `accepted` は来ない。時間切れで REVIEW。最後に取得したフレームのコピーを候補として PSRAM に保持する |
| REVIEW | 候補フレームを表示し「保存する」「撮り直す」。保存 → UPLOADING、撮り直す → ANNOUNCE（新たな10秒） |
| UPLOADING | 「写真を準備中」を表示し、`captured.wav` を再生。edge 無しなので即 PHOTO_QR（固定 URL）。実装は `EdgeClient::uploadDecision()` の戻りで分岐できる形にしておく |
| PHOTO_QR | 「写真を保存」+ QR(`FIXED_PHOTO_URL`) + 削除予定時刻（ステップ1では「--:--」）+「次へ」「撮り直す」 |
| X_QR | 「保存した写真をXに添付してね」+ QR(`FIXED_SHARE_URL`) +「戻る」「終了」。終了 → IDLE（首を neutral に戻す） |
| ERROR | 理由 +「再試行」「終了」。カメラ初期化失敗・サーボ初期化失敗で入る |

ルール（spec §4, §8 から）:

- 状態ごとの経過時間は `millis()` の差分（unsigned）で測る。CAPTURE の 10 秒は CAPTURE に入った時刻を基準にする。
- 起動指示（タッチ）は IDLE でだけ受ける。それ以外の状態のタッチは各画面のボタンとしてだけ扱う。
- セリフ再生中はマイクを止める。ステップ1ではマイクを使わないが `audio.speakerOn()` / `audio.micOn()` の切替 API を先に作る（K151 はマイクとスピーカーを同時に使えない）。
- 首が動いている間（`Motion.isMoving()`）に取得したフレームは候補にしない。
- 状態遷移ごとに `ESP_LOGI("app", "state %s -> %s (%lu ms)")` を出す。
- 撮り直しや終了で候補フレームは解放する。

## 5. 各モジュールの要点

### camera（hal/camera）

- 公式例どおり `M5.In_I2C.release()` の後 `esp_camera_init()`。設定は公式例の pin と `PIXFORMAT_RGB565` / `FRAMESIZE_QVGA` / `CAMERA_FB_IN_PSRAM` / `fb_count = 2` をそのまま使う。
- `grab()` は `camera_fb_t*` を返し、使い終わったら必ず `release()`。プレビューは `M5.Display.pushImage(0, 0, 320, 240, (uint16_t*)fb->buf)` で全面描画してから文字を重ねる。
- 候補フレームの保持は `heap_caps_malloc(len, MALLOC_CAP_SPIRAM)` に memcpy。
- 初期化順序: `M5StackChan.begin()`（M5.begin + 頭部タッチ + IOエキスパンダ + サーボ）を先に済ませてからカメラを begin する。**カメラ SCCB と CoreS3 内部 I2C（画面タッチ・電源・オーディオコーデック）は同じ GPIO 11/12 を使う**ので、カメラ初期化後も画面タッチ・スピーカー・サーボが動くかが最大の実機リスク。動かない場合の切り分けとしてビルドフラグ `PHOTOBOOTH_NO_CAMERA`（カメラを初期化せずプレビュー枠だけ描く）を用意する。

### head（hal/head）

- 内部単位は BSP と同じ 1/10 度。`config::HEAD_*` で `[X_MIN, X_MAX]`, `[Y_MIN, Y_MAX]` にクランプする。範囲外を要求されたら端で止める。
- `nudge(dx, dy)`: 1 回の変化量を `±HEAD_STEP_MAX` に制限し、前回指示から `HEAD_STEP_INTERVAL_MS` 未満なら無視する。`moveTo(x, y)` はクランプ後に `Motion.move(x, y, HEAD_SPEED)`。
- `neutral()` は `moveTo(HEAD_X_NEUTRAL, HEAD_Y_NEUTRAL)`。BSP の `goHome()` は (0, 0) へ動くが、Y=0 が「下向き」なので使わない。
- 起動時に `neutral()` してから待つ。IDLE に戻るときも `neutral()`。
- 校正値（符号・neutral）は config にだけ書き、コードに固定値を埋め込まない。

### audio（hal/audio）

- 埋め込みシンボルは `extern const uint8_t announce_wav_start[] asm("_binary_data_announce_wav_start");` と `_end`。
- `playAnnounce()` は `M5.Speaker.playWav(start, end - start)`。`isPlaying()` を App が監視して完了を判定する。
- `speakerOn()` = `M5.Mic.end(); M5.Speaker.begin(); M5.Speaker.setVolume(config::SPEAKER_VOLUME);`、`micOn()` はその逆。
- シャッター音は `captured.wav`。

### input（hal/input）

- `poll()` が 1 回の `M5StackChan.update()` を呼び、`Event { kind, x, y }` を返す。kind は `None / ScreenTap / HeadTap`。
- 画面タップは `M5.Touch.getDetail().wasClicked()`、頭部は `M5StackChan.TouchSensor.wasClicked()`。
- ボタン判定は ui 側の矩形と `(x, y)` を App が突き合わせる（input はボタンを知らない）。

### ui（ui/screens, ui/widgets）

画面は 320×240。共通レイアウト:

- 上 32px: タイトル帯（状態名の日本語）。
- 下 48px: ボタン帯。ボタンは最大 2 つ、幅は等分、間隔 8px。ラベルは `lgfxJapanGothic_20`。
- QR 画面: QR を左に 168×168（余白込み、モジュールが 160px 以上になる version にする）、右の 144px 幅に説明文と削除時刻。
- IDLE の顔: 黒背景に白の目 2 つ（円）と口（弧）。3 秒ごとに 150ms 目を閉じる。凝らない。
- プレビュー: 全面にフレームを描き、右上に人数（ステップ1は「--」）と残り秒数を大きく（`lgfxJapanGothic_40` 相当）重ねる。
- ちらつき防止: 静的画面は状態に入ったとき 1 回だけ描き、変化する部分（残り秒数、目の開閉）だけ `fillRect` で更新する。プレビュー中はフレーム描画後に文字を重ねるので毎フレーム描き直しで良い。

### edge（edge/edge_client.h, edge/null_edge.h）

```cpp
struct FrameResult { bool valid; uint8_t face_count; uint8_t target_face_count;
                     bool all_eyes_open; bool all_smiling; int servo_dx; int servo_dy; bool accepted; };
class EdgeClient {
 public:
  virtual ~EdgeClient() = default;
  virtual bool isOnline() = 0;
  virtual void sessionStart(const Session&) = 0;
  virtual bool sendFrame(const Session&, const camera_fb_t&) = 0;   // 非同期。結果は pollResult
  virtual bool pollResult(FrameResult& out) = 0;
  virtual void sessionTimeout(const Session&) = 0;
  virtual void reviewDecision(const Session&, bool save) = 0;
  virtual bool pollPhotoReady(String& photo_url, String& share_url, String& expires_at) = 0;
  virtual void sessionCancel(const Session&) = 0;
};
```

`NullEdge` は `isOnline()=false`、他は何もしない。App はこの API だけを使う。イベント名は `docs/spec.md` §8 と一致させる。

## 6. main.cpp の初期化順

1. `M5StackChan.begin()`（内部で `M5.begin()`）。
2. `audio.speakerOn()`。
3. `head.begin()` → `neutral()`。失敗（`Motion` が動かない）は ERROR ではなくログ + 画面の警告に留め、固定カメラとして続行（spec §9 サーボエラー）。
4. `camera.begin()`。失敗は ERROR 状態で「カメラ初期化失敗」+「再試行」。
5. `app.begin()` → IDLE。

`loop()` は `input.poll()` → `app.update(event, millis())` → `delay(1)`。`app.update` の中でプレビューを描く。目標 2 fps 以上（spec §6.1）、実測をログに残す。

## 7. 受け入れチェック（PR テンプレートに転記）

- [ ] 起動後 5 秒以内に IDLE 画面が出る。
- [ ] 画面タッチと頭部タッチのどちらでも撮影が始まる。撮影中の再タッチで二重起動しない。
- [ ] ANNOUNCE でセリフが最後まで鳴り、鳴り終わってから COMPOSE に進む。
- [ ] COMPOSE で首が左右に小さく動いて正面に戻る。動作範囲が config の上限を越えない。
- [ ] CAPTURE のプレビューが動き、残り秒数が 10→0 で減り、10 秒で REVIEW に入る。
- [ ] REVIEW の「撮り直す」で ANNOUNCE に戻り、「保存する」で PHOTO_QR へ進む。
- [ ] PHOTO_QR / X_QR の QR を iPhone または Android の標準カメラで読める。
- [ ] X_QR の「終了」で IDLE に戻り、首が正面に戻る。
- [ ] カメラ初期化後も画面タッチ・スピーカー・サーボが動く（I2C 共有の確認）。
- [ ] シリアルログに状態遷移と所要時間が出る。写真バイト列はログに出ない。

## 8. 実機で確認して config に反映する値

- サーボ X/Y の正方向と neutral（`HEAD_X_NEUTRAL`, `HEAD_Y_NEUTRAL`）。
- カメラ画像の左右反転の有無（プレビューが鏡像でないこと）。
- 実際の fps とプレビューのちらつき。
- QR の読み取りやすい version / サイズ。
