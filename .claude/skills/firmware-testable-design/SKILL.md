---
name: firmware-testable-design
description: firmware/ の自作アプリ (main/apps/) を書く・直すときの設計と検証の約束。ハードに依存しないロジックをハードとの接点から分け、ホストで走るテストを書く。アプリの追加、flow/game/net のロジック変更、テストの追加、「テストできるように分けたい」という依頼で使う。
---

# firmware のロジックをテストできる形に保つ

このリポジトリの `firmware/`（M5Stack 純正ファーム + 自作アプリ）で、自作アプリ `main/apps/app_*/` と共通部品 `main/apps/shared/` を書くときの約束。
目的は「実機でしか確かめられないコード」を減らすこと。実機は 1 台しかなく、まれなタイミングの不具合（例: 特定の 1 フレームで止めたときだけ停止先がずれる）は実機では再現も発見もできない。

## 1. 2 つの層に分ける

| 層 | 置き場所 | include してよいもの | 検証 |
| --- | --- | --- | --- |
| ロジック（ハード非依存） | `game/`、`flow/` の判断部分、`net/` の応答の解釈、形式の解析 (`jpeg_info`, `wav`) | 標準ライブラリと同じ層のヘッダだけ | `firmware/tests/` のホストテスト |
| ハードとの接点 | `hw/`（音・入力・首・カメラ・LED）、`view/`（LVGL）、`net/` の通信とタスク、`app_*.cpp`（Mooncake） | `<hal/hal.h>`、FreeRTOS、LVGL、`mooncake_log`、`esp_*` | 実機 |

ロジック層のファイルは **`<hal/hal.h>`、`freertos/*`、`lvgl.h`、`smooth_lvgl.hpp`、`mooncake*.h`、`esp_*.h` を include しない**。これが守られているかは `grep -l` で機械的に確かめられる。

ロジック層の書き方:

- 時刻 (`GetHAL().millis()`)、乱数 (`esp_random()`)、入力イベントは **引数で渡す**。中で取りに行かない。例: `Slot::advance(uint32_t now_ms)`、`Slot::start(int r0, int r1, int r2)`。
- 結果は戻り値か、問い合わせ用のメソッドで返す。ハードへの副作用（LED を光らせる、音を鳴らす）はロジックの外で、戻り値を見て行う。
- ログを出したければ、戻り値に理由を含めて呼び出し側で出す（`mclog` をロジック層に入れない）。例: `wav::parse()` は `Status` を返し、`Audio::parseWav()` がそれを見てログを出す。
- 立ち上がり・立ち下がりのように「呼び出しの間に起きたこと」は、ラッチして読める形にする（例: `Slot::takeReachStarted()`）。呼び出し側が毎回ポーリングする前提にしない。

接点層の書き方:

- ロジックを呼ぶだけの薄い層にする。判断を書かない。
- ロジックが接点を呼ばなければならないときは、小さな純粋仮想クラス（メソッド 2〜4 個）で受け、テストでは偽物を渡す。既存の例: `net::EdgeClient`（実装 `HttpEdgeClient` / `NullEdge`）。
- lvgl_cpp の部品は型どおりに `std::unique_ptr` で持つ（`Object*` で持って delete しない: ヒープ破壊の前例あり）。LVGL を触るところは `LvglLockGuard`。

既存コードの現状（2026-10 時点）: `app_roulette/game/slot.cpp` はこの形。`app_photobooth/flow/flow.cpp`（1,100 行超）と `net/http_edge_client.cpp` はロジックと接点が混ざっていて未テスト。これらを触るときは、触る部分だけでもロジックを切り出してテストを足す（全体の作り直しは別タスク）。

## 2. 何をテストし、何を実機に任せるか

テストを書く:

- 計算と状態遷移（位置・判定・タイムアウト・再試行回数・応答の解釈）。
- まれなタイミングでしか起きないこと（境界ちょうど、同一フレームでの複数イベント、長い停止のあとの追いつき、`millis` の一周）。
- 見ても正解が分からないこと（float の誤差、ヘッダ解析、バイト列の読み出し）。
- 不具合を直したときは、その不具合を再現するテストを必ず足す（直す前に失敗することを確かめる）。

実機に任せる（テストを書かない）:

- 見れば分かること（描画位置、色、LED の明るさ、音量、タップの反応）。
- ハードの偽物を作らないと書けないテスト。偽物の上で通っても本物で動く保証にならない。

## 3. テストの書き方

- 置き場所: `firmware/tests/<対象>_test.cpp`。1 ファイル 1 対象。
- 形式: 既存の `roulette_slot_test.cpp` と同じ。フレームワークは使わず、`expectEqual` / `expectTrue` と `main()` で書く（CI の依存を増やさない）。失敗時は何を期待して何が来たかを出して `exit(1)`。
- 登録: `firmware/tests/CMakeLists.txt` に `add_executable` + `add_test`。対象の `.cpp` はテストから直接ソースとして足す（ライブラリ化しない）。
- 実行:

  ```console
  cd firmware
  cmake -S tests -B /tmp/stackchan-tests && cmake --build /tmp/stackchan-tests && ctest --test-dir /tmp/stackchan-tests --output-on-failure
  ```

  macOS でリンクに失敗するときは `SDKROOT=/Library/Developer/CommandLineTools/SDKs/MacOSX26.sdk` を付ける（MacOSX27 SDK の `.tbd` を読めない環境がある）。ビルド先はリポジトリの外。
- CI: `.github/workflows/ci.yml` の `firmware-tests` が `firmware/main/**` か `firmware/tests/**` の変更で走る。

## 4. 変更を出す前に

1. ロジック層に禁止 include が入っていないことを確かめる:

   ```console
   grep -rlE '<hal/hal\.h>|freertos/|lvgl\.h|smooth_lvgl|mooncake|esp_[a-z_]+\.h' firmware/main/apps/*/game firmware/main/apps/shared/wav.* firmware/main/apps/app_photobooth/view/jpeg_info.* firmware/main/apps/app_photobooth/hw/head_logic.* firmware/main/apps/app_photobooth/net/edge_parse.* firmware/main/apps/app_photobooth/net/edge_types.h firmware/main/apps/app_photobooth/net/edge_url.* firmware/main/apps/app_photobooth/net/link_logic.* firmware/main/apps/app_photobooth/flow/time_format.* firmware/main/apps/app_roulette/hw/light_pattern.*
   ```

   （ロジック層のディレクトリを足したらここにも足す。）
2. ホストテストを通す（§3）。
3. `idf.py build` を通す。警告を増やさない。新しいソースを足したら `idf.py reconfigure`（GLOB の再評価）。
4. PR テンプレートの実機チェックリストを埋める。実機で確かめていない項目は「未検証」と書く。
