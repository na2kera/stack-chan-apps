# app_roulette 設計: 技育スロットを純正ファームのアプリとして移植する

移植元は Moddable の MOD [na2kera/stack-chan-roulette](https://github.com/na2kera/stack-chan-roulette)（`main` = PR #6 まで。`miniapp.ts` がゲーム本体、`mod.ts` が頭部タッチ・LED・発話）。
Moddable のコードはそのまま載らないので、`firmware/main/apps/app_roulette/` に C++（Mooncake + LVGL）で書き直す。
あわせて、app_photobooth 専用だった音声再生と入力を共通の場所へ切り出し、2 つのアプリで使う。

## 1. ゴールと非ゴール

ゴール:

- ランチャーのアプリ一覧の最後（Photobooth の次）に「Slot」が並び、開くとスロットの待機画面になる。
- 遊び方は移植元と同じ: 画面または頭部タップで 3 リールが回り、画面の左／中央／右をタップしてそのリールを止める。中央の行（ペイライン）に同じロゴが 3 つ揃えば当たり。
- 演出も移植元と同じ: 開始時に「スタート」の発話と約 1.2 秒の虹色 LED、リーチ（止まった 2 リールの中央が一致し、残り 1 つが回っている）で黄色 LED と「リーチ」の発話。全リール停止時とアプリを閉じたときに消灯する。
- 画面下端から上へスワイプ（純正のホームインジケータ）でアプリを閉じ、ランチャーに戻る。
- app_photobooth の動作は変えない（共通化は置き場所と名前空間の変更だけ）。

非ゴール:

- 当たり専用の効果音・首振りなど、移植元に無い演出の追加。
- edge / gallery との連携。Wi-Fi は使わない。
- 移植元リポジトリ（Moddable 版）の変更。

## 2. 移植元との対応

| 移植元 | このアプリ |
| --- | --- |
| Piu `Port.drawTexture`（拡大縮小なし、60x60） | LVGL の `lv_image`。全画面を使うので 72x72 に作り直す（§5） |
| ミニアプリ領域 320x196（上にホストのタブ） | 全画面 320x240。タブは無い（移植元 issue #2 はこれで解消） |
| `port.interval = 33` ms ごとの `onTimeChanged` | `onRunning` で `GetHAL().millis()` を見て 33 ms の固定ステップを進める（§4 game） |
| xorshift32（Compartment で `Math.random()` が使えないため） | `esp_random()`（game には乱数を引数で渡す） |
| ホストの TTS `audio.say('スタート' / 'リーチ')` | 純正ファームにローカルの TTS は無い。macOS の `say` で作った WAV を埋め込み、共通の `Audio` で鳴らす |
| 頭部 LED `lightRainbow('head')` / `lightOn('head', 24, 18, 0)` | `GetHAL().setRgbColor(index, r, g, b)` + `refreshRgb()`（12 個。0〜5 が左、6〜11 が右）。虹色は自前で回す |
| 頭部タップ（`release` かつ `tap`） | 共通の `Input`（Press → Release、スワイプは捨てる）の `HeadTap` |
| フォント `k8x12-12` | 20px の日本語サブセットフォントを生成（§5） |

## 3. 共通化（`firmware/main/apps/shared/`）

app_photobooth の `hw/audio`・`hw/input` と素材生成スクリプトを、アプリに依存しない形で `main/apps/shared/` に移す。
純正の `main/apps/common/`（upstream のファイル）には手を入れない。名前空間は `shared::hw`。

```
main/apps/shared/
├── hw/
│   ├── audio.h/.cpp   # WAV (24 kHz / mono / 16-bit) の再生タスク。photobooth/hw/audio を移動
│   └── input.h/.cpp   # 頭部タップ・画面タップ・ボタンを Event にしてキューに積む。photobooth/hw/input を移動
└── tools/
    ├── gen_font.sh    # 文字列ヘッダから非 ASCII 文字を拾って LVGL フォントを作る (引数で入出力を指定)
    └── make_voice.sh  # say + ffmpeg で 24 kHz / mono / 16-bit の WAV を作る (引数で出力先と文言を指定)
```

### Audio の変更点

今の `photobooth::hw::Audio` は `enum class Clip` と `pb_assets.h` の埋め込みシンボルを直接持っている。これを分ける。

- `shared::hw::Audio`: `begin()` / `end()` / `play(const Pcm&)` / `isPlaying()` / `stop()` と、`static Pcm parseWav(start, end, name)`（public）。どの WAV があるかは知らない。再生タスク・切り離し（`g_detached`）・出力の無効化の取り決めは今のまま動かす。
- app_photobooth 側: `hw/clips.h/.cpp` に `enum class Clip` と、`begin` 時に 4 本を `parseWav` して持つ薄い表（`photobooth::hw::Clips`）を置く。`flow.cpp` の `audio_.play(hw::Audio::Clip::X)` は `audio_.play(clips_.get(Clip::X))` 相当に置き換える。`play` の戻り値（WAV が不正なら false）と呼び出し順は変えない。
  （実装: `Clips` は `AppPhotobooth` が持ち、`Audio::begin()` の直前に `Clips::begin()` を呼んで `Flow` のコンストラクタに渡す。WAV を調べる順番とログは以前と同じ）
- ログのタグは `PB-Audio` → `Audio`、`PB-Input` → `Input`。

### Input の変更点

名前空間と置き場所だけ変える（`Event::Kind` は `ScreenTap` / `HeadTap` / `Button` のまま）。app_roulette はリールの列を `Button` の `index`（0, 1, 2）で受ける。

### スクリプトの変更点

- `shared/tools/gen_font.sh <strings.h> <出力.c> <px> [--ascii]`（photobooth の数字フォントのため、strings.h の代わりに `--symbols <文字>` も受ける）、`shared/tools/make_voice.sh <出力ディレクトリ> <名前> <文言> [<名前> <文言>...]` を本体にする。
- `app_photobooth/tools/gen_font.sh`・`make_voice.sh` はそれを呼ぶだけにする。**作り直した `pb_font_*.c` と `assets/voice/*.wav` が今のファイルとバイト単位で同じ**であることを確かめる（違ったら素材はコミットせず、スクリプトだけ直す）。

## 4. アプリ構成（`firmware/main/apps/app_roulette/`）

```
app_roulette/
├── app_roulette.h/.cpp    # AppAbility。onOpen で初期化、onRunning でゲームと演出を進める、onClose で後始末
├── game/
│   └── slot.h/.cpp        # リールとゲームの状態。LVGL / HAL / FreeRTOS に依存しない (ホストでテストする)
├── hw/
│   └── lights.h/.cpp      # 頭部 LED の演出 (虹色・リーチの黄色・消灯)
├── view/
│   ├── view.h/.cpp        # 画面。リール 3 本・ペイライン・状態の 1 行・タップ領域
│   └── strings.h          # 画面の文字列 (フォント生成の入力)
├── assets/
│   ├── rl_assets.h        # フォント・アイコン・リール画像・WAV の宣言
│   ├── rl_reel.c          # 72x288 の RGB565 (tools/make_reel.py で生成)
│   ├── rl_font_jp_20.c    # tools/gen_font.sh で生成
│   ├── icon_roulette.c    # 188x150 RGB565A8 (tools/make_icon.py で生成)
│   ├── src/*.svg          # ロゴの元データ (移植元 assets/src をコピー)
│   └── voice/rl_start.wav, rl_reach.wav
└── tools/
    ├── make_reel.py       # SVG → rl_reel.c
    ├── make_icon.py       # ランチャー用アイコン
    ├── gen_font.sh        # shared/tools/gen_font.sh を呼ぶ
    └── make_voice.sh      # shared/tools/make_voice.sh を呼ぶ
```

名前空間は `roulette`（`roulette::game` / `roulette::hw` / `roulette::view`）。

### game（純粋なロジック）

移植元 `miniapp.ts` の `Reel` と `GeekSlotBehavior` の状態部分をそのまま移す。値は 60px を 72px に換算する（比 1.2）。

| 定数 | 移植元 (60px) | このアプリ (72px) |
| --- | --- | --- |
| `kSymbolSize` | 60 | 72 |
| `kSymbolCount` / `kStripHeight` | 4 / 240 | 4 / 288 |
| `kFrameMs` | 33 | 33 |
| `kSpinSpeed`（px/フレーム） | 13 | 15.6 |
| `kStopSlip` | 30 | 36 |
| `kStopMinSpeed` | 3 | 3.6 |
| `kStopEasing` | 0.32 | 0.32 |

- `Reel`: `position`（float）、`phase`（Spinning / Stopping / Stopped）、`target`。`start(symbol)` / `requestStop()` / `update()` は移植元と同じ式。
- `paylineSymbol(position) = (round(normalize(position) / kSymbolSize) + 1) % kSymbolCount`。
- `Slot`:
  - `phase()`: Ready / Spinning / Result。`win()`、`reach()`、`winSymbol()`。
  - `start(r0, r1, r2)`: 3 リールの開始シンボル（0〜3）を受けて回し始める。乱数はアプリ側が `esp_random() % 4` で渡す。
  - `stopReel(index)`: Spinning のときだけ効く。
  - `step()`: 1 フレーム進める。全リール停止で Result に移り、`win` を決める。Spinning 中は「止まったリールがちょうど 2 つで、その中央のシンボルが一致」のとき `reach = true`、それ以外は false（移植元 `onTimeChanged` と同じ。同じフレームで 3 つ止まったらリーチにせず結果へ）。リーチの立ち上がりは `takeReachStarted()`（読むと消える）でも取れる。
  - 回転中の位置は 0.1 px の格子に丸める。`15.6f` を足し続けた誤差が残ると、シンボルの境目で止めたときに `requestStop` の `ceil` が 1 つ先を選ぶため（移植元は整数の 13 px で誤差が出ない）。
  - `advance(now_ms)`: 前回からの経過を 33 ms 単位で `step()` に変換する。1 回の呼び出しで進めるのは最大 5 ステップ（アプリが止まっていた後に一気に進めない。上限で切ったときは残りの遅れを捨てる）。Spinning 以外では時刻だけ更新する。最初の呼び出しと `start()` 直後の呼び出しも時刻を覚えるだけ（待機中の経過を回り始めに乗せない）。
- 初期表示は移植元と同じくリール i の位置を `i * kSymbolSize` にずらす。

### hw/lights

- `startRainbow(now_ms)`: 1.2 秒間、12 個の LED に色相をずらした虹色を出す。更新は 50 ms ごと（I2C の書き込みを毎フレームしない）。明るさは移植元の黄色（24, 18, 0）と同程度に抑える（最大成分 24）。
- `setReach(bool)`: true で全 LED を (24, 18, 0)。虹色の途中なら打ち切って黄色にする（移植元と同じ優先順位）。false で消灯。
- `off()`: 消灯。`update(now_ms)`: 虹色の更新と 1.2 秒後の消灯。
- 同じ色を続けて書かない（状態が変わったときだけ `refreshRgb()`）。

### view

- `lv_screen_active()` の上に 1 枚のルート（`Container`）を作り、その中に部品を置く。lvgl_cpp の部品は `std::unique_ptr` で型どおりに持つ（`Object*` に入れて delete しない）。LVGL を触る関数は中で `LvglLockGuard` を取る。
- リール: 72x216 の枠なしコンテナ（はみ出しは切る）に、`rl_reel` の `lv_image` を 2 枚、縦に 288px ずらして入れる。位置 `p`（0〜288）に対して 1 枚目を `y = -p`、2 枚目を `y = 288 - p` に置くと、窓がシートの末尾をまたいでも続きが見える。位置が変わったリールだけ動かす。
- タップ領域: リールの窓と同じ高さ（y = 0〜216）で画面を横に 3 等分した透明の領域。`LV_EVENT_PRESSED` で `input.pushButton(screen_id, 列)` を積む（移植元は `onTouchBegan` = 押した瞬間）。下端 24px（状態の行）はタップ領域にしない（ホームインジケータの上スワイプの始点と重ねないため）。
- 終了: 純正の `view::create_home_indicator([this] { close 要求 })` / `update_home_indicator()` / `destroy_home_indicator()` を使う（app_dance と同じ）。色はアプリのテーマ色に合わせる。

### app_roulette

- `onOpen`: `shared::hw::Input`・`shared::hw::Audio`・`Lights`・`View`・`game::Slot` を作る。WAV 2 本を `parseWav` する。音声が使えなくてもゲームは続ける。
- `onRunning`:
  1. `input.poll()` を 1 件処理する。`HeadTap` は Spinning 以外なら開始。`Button(i)` は Spinning なら `stopReel(i)`、それ以外なら開始。
  2. 開始時: `esp_random()` で 3 つのシンボルを決めて `slot.start()`、`lights.startRainbow()`、再生中でなければ「スタート」を鳴らす（移植元の「発話中に再スタートしたら重ねない」）。
  3. `slot.advance(now)`。リーチの立ち上がり（`takeReachStarted()`。1 回の `advance` の中でリーチから全停止まで進んでも取りこぼさない）で `lights.setReach(true)` と「リーチ」の再生（1 回だけ）、立ち下がりで `setReach(false)`。Result に移ったら `lights.off()`。
  4. `lights.update(now)`、`view.render(slot)`、`view::update_home_indicator()`。
  5. 終了要求があれば `close()`。
- `onClose`: 消灯、音声と入力を止める、ホームインジケータと画面を壊す。首・カメラ・Wi-Fi は触らない。
- ランチャー: 名前は「Slot」、テーマ色は当たりの色と同じ `0xFFCC33` 系ではなく、背景に合わせた紫系（§5）。

### 登録（純正への変更）

- `main/apps/apps.h` に include 1 行、`main/main.cpp` の `installApp` を **AppPhotobooth の後**に 1 行（install 順の番号に依存する `requestWarmReboot(index)` を壊さない）。
- `main/CMakeLists.txt`: `file(GLOB ROULETTE_VOICES apps/app_roulette/assets/voice/*.wav)` を足して `EMBED_FILES` に加える。埋め込みシンボルはファイル名から決まるので、WAV の名前は `rl_` を付けて photobooth と重ならないようにする。
- `apps/*.cpp` / `*.c` は既存の GLOB が拾うので、ソースの追加に CMake の変更は要らない。

## 5. 画面（320x240）

移植元は 320x196 の領域に 60x60 のロゴを置いていた。全画面を使い、ロゴを 72x72（1.2 倍）にする。色は移植元のまま。

```
x:  0        40      112 124     196 208     280      320
y=0  ┌────────┬────────┬─┬────────┬─┬────────┬────────┐
     │        │ reel 0 │ │ reel 1 │ │ reel 2 │        │
y=72 │  ──────┼────────┼─┼────────┼─┼────────┼──────  │  ← ペイライン上 (2px)
     │        │  中央  │ │  中央  │ │  中央  │        │
y=144│  ──────┼────────┼─┼────────┼─┼────────┼──────  │  ← ペイライン下 (2px)
     │        │        │ │        │ │        │        │
y=216├────────┴────────┴─┴────────┴─┴────────┴────────┤
     │            状態の 1 行 (20px フォント)            │
y=240└────────────────────────────────────────────────┘
```

| 部品 | 位置・大きさ | 色 |
| --- | --- | --- |
| 背景 | 全面 | `#12121a` |
| リールの窓 | x = 40, 124, 208、y = 0、72x216 | 移植元の PNG と同じ見え方（暗い下地 `#0d0d12` に白地の丸いロゴ） |
| リールの枠 | 窓の外側 1px（窓の `outline`。上辺は画面外） | 回転中 `#9a9ab8`、停止 `#5a5a70` |
| ペイライン | x = 26〜294（幅 268）、y = 71〜72 と 143〜144 | 通常 `#8a7a3a`、当たり `#ffcc33` |
| 状態の行 | y = 216〜240、中央寄せ | 通常 `#c8c8d8`、当たり `#ffcc33` |

状態の行の文言（20px で 320px に収まる長さにする。移植元の文言は 12px 前提で長いので短くする）:

| 状態 | 文言 |
| --- | --- |
| Ready | `タップでスタート` |
| Spinning | `タップで止める` |
| Result（当たり） | `<ロゴ名> がそろった！`（技育展 / 技育祭 / 技育博 / 技育CAMP。「揃」はフォント元の `puhui-common.ttf` に無いのでかな） |
| Result（はずれ） | `はずれ タップでもう一度` |

- ロゴの並び（シートの上から 0=技育展, 1=技育祭, 2=技育博, 3=技育CAMP）は移植元と同じ。`make_reel.py` の並びと `strings.h` のロゴ名を揃える。
- `rl_reel.c` は移植元 `tools/build-assets.sh` と同じ作り方を 1.2 倍にしたもの: `rsvg-convert` で各 SVG を 67x67（移植元 56/60 の比率）にラスタライズし、`#0d0d12` の 72x72 のセルの中央に合成して RGB565 の C 配列にする（透過は持たない）。
- ランチャーのアイコンは 188x150 / RGB565A8（純正・photobooth と同じ形式）。図柄はスロットの 3 リール。テーマ色は `0x6C5CE7` 系の紫（ランチャーの背景色。純正アプリや Photobooth の黄色と被らないもの）。

## 6. テスト

- `firmware/tests/` に `roulette_slot_test.cpp` を足す（既存の `motion_math_test` と同じく、ホストでビルドして `ctest`）。確かめること:
  - `paylineSymbol`: 位置 0 / 72 / 216 / 287.9 と負の位置。
  - `requestStop`: 最低 `kStopSlip` 滑ってシンボルの区切りで止まり、止まった位置が `kSymbolSize` の倍数。
  - 3 リールが揃えば `win`、揃わなければはずれ。
  - リーチ: 止まった 2 つが一致して 1 つ回っている間だけ true。停止順（0-1、0-2、1-2）によらない。全停止で false。
  - Spinning 以外では `stopReel` が効かない。`advance` は 1 回で最大 5 ステップ、`start()` 直後は進めない。
  - シンボルの境目（30 ステップ後 = 位置 180）で止めると停止先が 216 になる。リーチの立ち上がりは全停止の後でも 1 回読める。
- 共通化の確認: photobooth の素材（フォント・WAV）がバイト単位で変わらないこと、`idf.py build` が通ること。
- 実機（ユーザーが確認する）: 下の受け入れチェック。

## 7. 実装順

1. 共通化（§3）。photobooth だけでビルドが通り、素材が変わらないことを確かめてコミット。
2. `game/` とホストのテスト。
3. 素材（リール画像・フォント・WAV・アイコン）と生成スクリプト。
4. `view/`・`hw/lights`・`app_roulette` と登録。`idf.py build`。
5. README（ルートと `firmware/README.md`）に app_roulette と共通部品を追記。

## 8. 受け入れチェック（PR に転記）

- [ ] `idf.py build` が通る
- [ ] `firmware/tests` の `ctest` が通る
- [ ] ランチャーの最後に「Slot」が出て、開くと待機画面になる
- [ ] 画面タップ・頭部タップのどちらでも回り始める
- [ ] 左／中央／右のタップで対応するリールが止まる
- [ ] 揃うと当たりの表示、揃わないとはずれの表示になり、次のタップで再開できる
- [ ] 開始時に「スタート」と虹色 LED（約 1.2 秒）、リーチで黄色 LED と「リーチ」、全停止で消灯
- [ ] 下端から上スワイプ → ホームボタンでランチャーに戻り、LED が消えている
- [ ] Photobooth が今までどおり一周動く（セリフ・シャッター音・頭部タップ・ボタン）
- [ ] 純正アプリ（AI エージェント・アバター・ダンス・設定）が開く

## 9. 実機で確認して決める値

- LED の明るさ（最大成分 24）と虹色の回る速さ。
- リールの速さ（`kSpinSpeed` 15.6 px/フレーム）。`onRunning` の呼ばれる間隔が 33 ms より粗い場合の見え方。
- 「スタート」「リーチ」の音量と長さ（仮音声は macOS の Kyoko）。
