# ロジックの切り出しとホストテスト ステップ1

目的: `firmware/` の自作アプリで「実機でしか確かめられないコード」を減らす。ハードに依存しないロジックをハードとの接点から分け、`firmware/tests/` のホストテストで確かめ、CI で走らせる。
約束事は `.claude/skills/firmware-testable-design/SKILL.md`（以下「skill」）。この設計書はその最初の適用で、**今の構造のまま切り出せるもの**だけを扱う。`flow/flow.cpp` 全体の作り直しは別ステップ（§7）。

## 1. ゴールと非ゴール

ゴール:

- skill に従って、次の 5 か所のロジックをハード非依存のファイルに切り出し、ホストテストを足す（§3〜§5）。
- `firmware/tests` を CI（`.github/workflows/ci.yml`）で走らせる。
- 既存の動作を変えない。切り出しは「同じ計算を別ファイルに移す」だけで、分岐・しきい値・ログの文言を変えない。

非ゴール:

- `flow/flow.cpp` の状態機械のテスト（接点 6 つの抽象化が要る。§7）。
- `http_edge_client.cpp` の通信・タスク・再試行まわり（応答の **解釈** だけを切り出す）。
- 画面（`view/`）と LED の書き込み、音の再生そのもの。

## 2. 置き場所と層の規則

| 切り出し先 | 層 | 名前空間 |
| --- | --- | --- |
| `main/apps/shared/wav.h/.cpp` | ロジック | `shared::wav` |
| `main/apps/app_photobooth/view/jpeg_info.h/.cpp`（既存のまま） | ロジック | `photobooth::view` |
| `main/apps/app_photobooth/hw/head_logic.h/.cpp` | ロジック | `photobooth::hw` |
| `main/apps/app_photobooth/net/edge_parse.h/.cpp` | ロジック | `photobooth::net` |
| `main/apps/app_photobooth/flow/time_format.h/.cpp` | ロジック | `photobooth::flow` |
| `main/apps/app_roulette/hw/light_pattern.h/.cpp` | ロジック | `roulette::hw` |

ロジック層のファイルは skill §1 の禁止 include（`hal/hal.h`、FreeRTOS、LVGL、mooncake、`esp_*`）を含まない。`config.h`（定数だけ）と `view/strings.h`（文字列だけ）は含んでよい。`edge_parse` だけは ArduinoJson に依存する（§4）。

## 3. 切り出し A・B: WAV と JPEG のヘッダ解析（作り直しなし）

### A. `shared::wav::parse`（実装済み）

- `wav.h/.cpp` は書いてある。`Audio::parseWav(start, end, name)` の本体を `wav::parse(start, end - start, kSampleRate, pcm)` の呼び出しに置き換え、`Status` を見て **今と同じ文言・同じタグ** でログを出し、`Audio::Pcm` に詰めて返す。`audio.cpp` の `le32` / `le16` は不要になるので消す。
- 戻り値・呼び出し側（`clips.cpp`、`app_roulette.cpp`）は変えない。
- テスト `firmware/tests/wav_test.cpp`: ヘッダをバイト列で組み立てるヘルパを書き、次を確かめる。
  - 正常（fmt → data）で `samples = data サイズ / 2`、`data` が本体を指す。
  - `fmt` の前に `LIST` など別のチャンクが挟まっても読める。奇数サイズのチャンクの詰め物（1 バイト）を飛ばす。
  - 短すぎる・`RIFF`/`WAVE` でない → `NotRiff`。
  - stereo / 8-bit / 16 kHz / format≠1 → `Unsupported`（`out.format` に値が入る）。
  - `data` が `fmt` より前、`data` チャンクが無い、サイズがバッファを超える → `NotFound`。

### B. `photobooth::view::readJpegInfo`（既存。テストだけ足す）

- テスト `firmware/tests/jpeg_info_test.cpp`: バイト列を組み立てて次を確かめる。
  - SOF0 / 8-bit / 3 成分 → `Ok`、幅・高さ・成分数が読める。APP0 (JFIF) や DQT を挟んでも読める。詰め物の `0xFF` が続いてもよい。
  - SOF2（プログレッシブ）、12-bit、4 成分 → `Unsupported`（`out` は埋まる）。
  - `nullptr`、4 バイト未満、SOI でない、セグメント長がバッファを超える、SOF の前に SOS/EOI、幅または高さが 0、SOF0 の長さが成分数と合わない → `Invalid`。
  - RSTn / TEM マーカー（長さなし）を飛ばす。

## 4. 切り出し C: edge の応答の解釈（`net/edge_parse`）

`http_edge_client.cpp` の中で JSON を `FrameResult` / `PhotoInfo` に変換している部分を純粋関数にする。通信・mutex・mailbox・ログ・統計はそのまま `EdgeWorker` に残す。

```cpp
namespace photobooth::net {
enum class ParseStatus : uint8_t { Ok, BadJson, Mismatch, BadResponse, BadPhotoUrl };

// frame の応答本文。session_id / frame_id が送ったものと違えば Mismatch。
ParseStatus parseFrameResult(const char* json, size_t len, const char* expect_sid, uint32_t expect_frame_id,
                             FrameResult& out);
// photo の応答本文。pending なら out.status = Pending で Ok。
// ready で photo_url / share_url / expires_at のどれかが空なら BadResponse、
// どれかが PhotoInfo の配列に収まらなければ BadPhotoUrl (reason もその場で入れる)。
// missing: BadResponse のとき欠けていた項目名 ("photo_url" など)。ログ用 (実装で追加。下記)。
ParseStatus parsePhotoInfo(const char* json, size_t len, PhotoInfo& out, const char** missing = nullptr);
Hint parseHint(const char* h);  // 既存の無名名前空間の関数を移す
}
```

- `error`（と不明な `status`）は `out.status = Error`、`reason` をコピー（無ければ `"error"`）して `Ok` を返す。`parseFrameResult` は `BadJson` / `Mismatch` のとき `out` を変えない。
- 実装で変えた点: `parsePhotoInfo` に省略可能な引数 `missing` を足した。今のログ `photo ready without {photo_url|share_url|expires_at}` はどの項目が欠けたかを出すが、`ParseStatus` と `PhotoInfo` だけでは呼び出し側に分からないため（欠けた応答で `PhotoInfo` に URL を入れておく案は、publish する内容が変わるので採らなかった）。

- `sendFrame()` の `deserializeJson` 〜 `fr.latency_ms` まで、`pollPhoto()` の `deserializeJson` 〜 `publishPhoto` の直前までを置き換える。ログ（`photo ready without …`、`setErrorOp`）は `ParseStatus` を見て **今と同じ文言** を `EdgeWorker` 側で出す。`clampU8` は `edge_parse.cpp` に移す。
- `edge_client.h` は `esp_heap_caps.h`（PSRAM アロケータ）を include している。`edge_parse.h` から `FrameResult` / `PhotoInfo` / `Hint` を使うには、これらの型を `net/edge_types.h`（純粋な型だけ）に移し、`edge_client.h` がそれを include する形にする。`PhotoInfo::reason` の値 `kSaveRetryExhausted` も `PhotoInfo` と一緒に移した。
- ArduinoJson: 実機では `components/ArduinoJson`（`repos.json` の v7.4.2、ヘッダのみ）。ホストテストでは `firmware/tests/CMakeLists.txt` で `../components/ArduinoJson` があればそれを、無ければ `FetchContent` で **同じ v7.4.2** を取る（CI は `fetch_repos.py` を実行しないため）。バージョンは `repos.json` と同じ値にし、コメントでそう書く。
- テスト `firmware/tests/edge_parse_test.cpp`:
  - frame_result の全項目が入る。`hint` の `closer` / `too_many` / 無し。`face_count` 300 → 255 に丸める。`latency_ms` 70000 → 65535。
  - `session_id` 違い・`frame_id` 違い → `Mismatch`。壊れた JSON → `BadJson`。項目が欠けていても既定値で `Ok`。
  - photo: `pending` → `Pending`。`ready` で 3 つ揃えば `Ready`。1 つ欠け → `BadResponse` (reason `bad_response`)。256 文字以上の URL → `BadPhotoUrl` (reason `bad_photo_url`)。`error` + `reason` → `Error` で reason がコピーされる。`reason` 無し → `"error"`。

## 5. 切り出し D・E・F

### D. 首の応答監視（`hw/head_logic`）

`Head`（`hw/head.cpp`）は、純正 `Motion` の呼び出しと「指示・監視・応答なし判定・一時停止・再開」の状態機械が同じクラスにある。後者は PR #15 で何度も直した場所で、実機では再現が難しい。

- `head_logic.h`: `Head` の状態（`target_x_` … `last_motion_ms_`）と `begin/update/command/nudge/moveTo/neutral/end` の判断をそのまま持つ `HeadLogic` を作る。`Motion` の呼び出しは次の小さなインターフェースに置き換える。

  ```cpp
  struct Servo {
      virtual ~Servo() = default;
      virtual void moveWithSpeed(int x, int y, int speed) = 0;
      virtual bool isMoving() = 0;
      virtual int currentX() = 0;
      virtual int currentY() = 0;
      virtual void stop() = 0;
  };
  ```

  `HeadLogic` は `Servo&` を受け取る。`motion().update()`、`setAutoTorqueReleaseEnabled`、`setAutoAngleSyncEnabled` は `Motion` 固有なので `Head`（接点）に残す。ログ（`mclog`）はロジック層に置けないので、`HeadLogic` は「何が起きたか」を戻り値（`enum class Event { None, Settled, NearTargetSettled, Paused, Faulted, Resumed }`）で返し、`Head::update()` が今と同じ文言で出す。`nudge` / `moveTo` のログも同様に `Head` 側。
- `Head` は `HeadLogic` と `MotionServo`（`Servo` の `Motion` 実装）を持つ薄い層になる。公開メソッドと戻り値は変えない。
- 実装で決めた細部（ログを今と同じ文言・同じ値で出すため）:
  - `HeadLogic::begin()` は状態の初期化だけ。`neutral` の指示は `Head::begin()` が `setAutoTorqueReleaseEnabled(false)` などの後に `Head::neutral()` で出す（`moveTo` のログを `Head` で出すため）。
  - `HeadLogic::end(now)` は `active` でなければ `false`。そうでなければ待ちを取り消し、異常でなければ neutral を指示して `true`。`Head::end()` はその後に `moveTo` のログ・`setAutoTorqueReleaseEnabled(true)`・`end:` のログを出す。
  - `update(now, Detail*)`: `NearTargetSettled` / `Paused` / `Faulted` のときの実際の角度と、置き換える前の目標を `Detail` で返す。経過時間は `Head` が `now - lastCommandMs()`、回数は `faultCount()` で取る。
  - `nudge(dx, dy, now, Step*)`: 実際に使った変化量（制限と `HEAD_MIN_STEP` の後）を `Step` で返す（`nudge d=(…)` のログ用）。
  - クランプは自由関数 `clampHeadX` / `clampHeadY`（`Head::moveTo` の `moveTo clamped` のログでも使う）。
  - `Head` は `logic_` が `servo_` を参照するのでコピーを禁止した（使っている所は `std::unique_ptr` で持つだけなので影響なし）。
  - 一時停止からの再開で待っていた指示を送るとき、`resume head commands …` のログは今は指示の **後** に出る（前は指示の前）。指示とログの中身は同じ。
  - 応答なしで首を止めた後の `nudge` は、切り出す前と同じく `true` を返す（`command` が何もしないので指示は出ない）。テストもこの動作を確かめる。
- テスト `firmware/tests/head_logic_test.cpp`（偽の `Servo` を使う）:
  - `nudge`: 間隔 `HEAD_STEP_INTERVAL_MS` 未満は false。±`HEAD_STEP_MAX` に制限。`HEAD_MIN_STEP` 未満は 0 扱い。可動域（`HEAD_X_MIN/MAX`、`HEAD_Y_MIN/MAX`）でクランプ。
  - 監視: `isMoving` が false を返したら `Settled`、`fault_count` が 0 に戻る。`HEAD_MOVE_TIMEOUT_MS` を過ぎて目標の近く（`HEAD_SETTLE_TOLERANCE`）なら `NearTargetSettled`、`stop()` は呼ばない。
  - 応答なし: 目標から離れたまま timeout → `stop()` が呼ばれ、`Paused`、目標が実際の角度に置き換わる。待っている間の `nudge` は false、`moveTo` は保留され `HEAD_FAULT_RETRY_MS` 後に `Resumed` で送られる。
  - `HEAD_FAULT_LIMIT` 回続けて応答なし → `Faulted`、以後 `command` が何もしない（`moveWithSpeed` が呼ばれない）、`isMoving()` false。
  - `lastMotionMs()`: 指示時刻と、`isMoving` true の最後の問い合わせ時刻。
  - `millis` の一周をまたいでも timeout 判定が正しい。

### E. LED の色の計算（`app_roulette/hw/light_pattern`）

`Lights` の `Mode`・時刻・色相計算を `LightPattern` に移し、`Lights` は「`LightPattern` が返した 12 色を、前回と違うときだけ `GetHAL()` に書く」だけにする。

- `LightPattern`: `startRainbow(now)`, `setReach(bool)`, `off()`, `bool colors(uint32_t now_ms, Rgb (&out)[12])`（出力が前回と変わったら true）。虹色の 1.2 秒経過での消灯、50 ms 間隔、リーチ優先はここ。
- 実装で決めた細部: `Rgb`・`hueToRgb`・`kMaxLevel` はテストのため `light_pattern.h` に出した。`Lights` の `startRainbow` / `setReach` / `off` は今までどおりその場で LED に書く（`colors()` を呼んで変わっていれば書く）。消灯・リーチの色は時刻によらないので、`setReach` / `off` は `Lights` が最後に受けた時刻で `colors()` を呼ぶ。LED ごとの「前回と同じなら書かない」は `Lights::show()` に残した。
- テスト `firmware/tests/light_pattern_test.cpp`: `hueToRgb` の 6 区間の端（0, 60, …, 300 と 359）、最大成分が `kMaxLevel` を超えない、虹色が 1200 ms で消える、50 ms 未満は再計算しない、虹色中の `setReach(true)` で全 LED が (24,18,0)、`setReach(false)` と `off()` で消える、`millis` の一周。

### F. 期限の表示（`flow/time_format`）

`flow.cpp` の `formatExpires(iso, out, len)` を `flow/time_format.h/.cpp` に移す（文言 `str::kExpiresUnknown` は `view/strings.h` から）。テスト `firmware/tests/time_format_test.cpp`: `"2026-09-30T22:00:00+09:00"` → `"22:00"`、短い・`T` が無い・`:` の位置違い・`nullptr` → `"--:--"`、`len` が小さいときに切り詰める。

## 6. CI

`.github/workflows/ci.yml` に `firmware-tests` ジョブを足す。

- `changes` のフィルタに `firmware: ['firmware/main/**', 'firmware/tests/**', '.github/workflows/ci.yml']`。
- `ubuntu-latest` で `cmake -S firmware/tests -B build-host-tests && cmake --build build-host-tests && ctest --test-dir build-host-tests --output-on-failure`。追加の依存は ArduinoJson の `FetchContent` だけ（§4）。
- `firmware/README.md` の「Host-side tests」に、テストの一覧と macOS の `SDKROOT` の注意を足す。
- 実装の確認: `components/` の無いコピー（`git archive`）で `FetchContent` の経路を通した。Linux の GCC では未実行（この Mac の Homebrew GCC は動かず、Docker も止まっている）。代わりに ESP-IDF の GCC 14.2（xtensa）で全テストを `-fsyntax-only -Wall -Wextra` にかけ、警告なしを確かめた。

## 7. 次のステップ（この PR には入れない）

- `flow/flow.cpp`: `Camera` / `Head` / `Audio` / `View` / `EdgeClient` を純粋仮想クラスで受けるようにして状態機械をテストする。`EdgeClient` は既にインターフェースなので、残り 4 つ。一度に全部ではなく、`updateCompose` / `updateCapture` / `handleResult` から。
- `EdgeWorker` の再試行・ポーリングの方針（save の回数、photo の待ち時間）を `edge_policy` として切り出す。
- `app_roulette.cpp` の入力の振り分け（`HeadTap` / `Button` → 開始か停止か）。

## 8. 受け入れチェック

- [x] skill §4 の `grep` でロジック層に禁止 include が無い（§2 の 6 ファイル + `net/edge_types.h`。`edge_parse` の ArduinoJson は除く。skill の grep にも足した）
- [x] `ctest` が全部通る（既存 2 本 + 新規 6 本）
- [x] `idf.py build` が通り、警告が増えない
- [ ] `git diff` で、切り出した関数のしきい値・分岐・ログの文言が変わっていない（実装者は確認済み。レビューで確認）
- [ ] 実機: photobooth の一周（首振り・応答なしの表示・QR）とスロットの LED が今までどおり（PR #19 と合わせて確認）
