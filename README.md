# stack-chan-apps

M5Stack StackChan K151 の純正ファームに追加する自作アプリ集。1 つ目は自動撮影アプリ (photobooth)。

photobooth は、StackChan に「写真撮りたい」と話しかける（またはタッチする）と、首を動かして全員を画角に収め、全員が目を開けて笑った瞬間の写真を撮り、ダウンロード用 QR と X 投稿用 QR を表示する自動撮影アプリ。

仕様は [docs/spec.md](docs/spec.md)、設計は [docs/design/](docs/design/) を参照。

## 構成

```
firmware/  純正ファーム (m5stack/StackChan の firmware/ を git subtree で取り込み) + 自作アプリ
           main/apps/app_photobooth/ が撮影アプリ。ルーレット (app_roulette) も今後ここに追加
device/    旧・独立ファーム (PlatformIO, Arduino-ESP32)。凍結: 参照用に残し、以後更新しない
edge/      PC で動かす Python サービス (音声認識・顔判定・首振り計算)   ※ステップ2以降
gallery/   公開 HTTPS の写真配布サービス                                 ※ステップ4以降
edge-cloud/ edge を Cloudflare Containers で動かす前段の Worker (PC なし構成)  ※ステップ5以降
docs/      仕様・設計・通信契約
```

現在の進捗: 純正ファーム内アプリ版のステップ2（[docs/design/fw-app-step2.md](docs/design/fw-app-step2.md)。app_photobooth を Wi-Fi で edge に繋ぎ、顔判定・首振り・本物の QR を動かす）を実装中。ステップ1（[docs/design/fw-app-step1.md](docs/design/fw-app-step1.md)）は実装済み。

## ビルドと書き込み（純正ファーム内アプリ版）

撮影アプリは `firmware/`（純正ファームの ESP-IDF プロジェクト）に入っている。純正のホーム画面・AI エージェント・既存アプリはそのまま残り、ランチャーに「Photobooth」が増える。詳細（フォークの差分、自動更新を止めた範囲、素材の作り直し）は [firmware/README.md](firmware/README.md) の「Photobooth fork」を参照。

```console
cd firmware
python3 ./fetch_repos.py                 # 初回のみ。依存 (mooncake, xiaozhi-esp32 など) を取得してパッチを当てる
source ~/esp/esp-idf-v5.5.4/export.sh    # ESP-IDF v5.5.4
idf.py set-target esp32s3                # 初回のみ
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

- 書き込みでつながらないときは、下の「書き込み」と同じくダウンロードモード（リセット長押し 2 秒 → 緑 LED）にする。
- 純正に戻すときは、下の「復旧」と同じく M5Burner で純正ファームを書き込む（手順は変わらない）。
- 純正 (upstream) の更新を取り込むときは `git subtree pull --prefix=firmware <upstream firmware-only ブランチ> --squash`（詳細は docs/design/fw-app-step1.md §2）。
- このビルドは AI エージェント起動時の自動更新をしない。SETUP から手動で更新すると純正に置き換わり、撮影アプリは消える。
- edge (PC) と繋ぐには `firmware/main/apps/app_photobooth/config_local.example.h` を `config_local.h` にコピーして接続先と共有鍵を書き、edge を `--host 0.0.0.0` で起動する（手順は [firmware/README.md](firmware/README.md) の「edge (PC) と繋ぐ」）。`config_local.h` が無い、または `idf.py -DPHOTOBOOTH_NO_EDGE=1 build` なら edge なしの単体動作（固定 URL の QR）になる。

以下の「準備」〜「音声素材の差し替え」は凍結した旧・独立ファーム（`device/`）の手順。

## 準備

### 必要なもの

- M5Stack StackChan K151 と USB-C ケーブル
- PlatformIO Core（`pio --version` が通ること）
- macOS で仮音声を作り直す場合のみ `ffmpeg`

### 初回セットアップ

```console
cd device
cp include/config.example.h include/config.h   # 必要なら値を編集
pio run                                          # 初回は pioarduino とツールチェーンを取得するので数分かかる
```

`include/config.h` が無いまま `pio run` すると、`tools/ensure_config.py` が `config.example.h` をコピーする（既存の `config.h` は上書きしない）。`config.example.h` に項目が増えたときは、手元の `config.h` にも同じ項目を足すこと（足りないとコンパイルエラーになる）。

カメラと内部 I2C の共有が原因と思われる不具合（カメラ初期化後にタッチ・スピーカー・サーボが効かない等）を切り分けるときは、`platformio.ini` の `build_flags` にある `-DPHOTOBOOTH_NO_CAMERA` のコメントを外してビルドする。カメラを初期化せず、プレビュー枠だけを描く。

通常はカメラが M5 の内部 I2C バスを共有する。無音を調査するときは `-DPHOTOBOOTH_AUDIO_DIAGNOSTICS` を有効にすると、起動時にカメラ初期化前・直後・Speaker 再初期化後の3回、テスト音とアンプ状態のログが出る。`-DPHOTOBOOTH_CAMERA_OWN_I2C` を追加すると PR #1 のバス初期化方式を再現できる。比較条件とログの読み方は [音声調査](docs/investigations/camera-speaker-i2c.md) を参照。

ボードパッケージとライブラリの版は `device/platformio.ini` で固定している。理由は同ファイルのコメントと [docs/design/step1-device.md](docs/design/step1-device.md) の「技術選定」を参照。

## 書き込み

**書き込む前に、下の「復旧」を一度読んでおくこと。** 純正ファームは上書きされる。

1. K151 を USB-C で接続する。
2. 本体のリセットボタンを約 2 秒長押しし、内部の緑 LED が点灯したら離す（ダウンロードモード）。
3. ポートを確認して書き込む。

```console
cd device
ls /dev/cu.usbmodem*
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor -b 115200 --port /dev/cu.usbmodemXXXX
```

書き込み後に自動で再起動しない場合はリセットボタンを短く押す。

## 起動と操作（ステップ1）

1. 起動すると顔と「写真を撮りたい、と言ってね」が出る。
2. 画面か頭の上をタッチすると「写真を撮るよ！ いい顔をしてね」と話し、首を少し動かしたあと 10 秒のカウントが始まる。
3. 10 秒後に候補写真と「保存する」「撮り直す」が出る。
4. 「保存する」で写真 QR、「次へ」で X 投稿 QR が出る。ステップ1では QR の中身は `config.h` の固定 URL。

## 復旧（純正ファームに戻す）

M5Stack 公式の M5Burner で StackChan の純正ファームを書き戻せる。

1. [M5Burner (macOS)](https://m5burner-cdn.m5stack.com/app/M5Burner-v3-mac-x64.dmg) を入れる（Windows / Linux は [公式ドキュメント](https://docs.m5stack.com/en/StackChan) 参照）。
2. M5Burner を開き、「Only Official」にチェックして `StackChan` を検索し、最新の純正ファームをダウンロードする。
3. K151 を USB-C で接続して電源を入れ、必要ならダウンロードモード（リセット長押し 2 秒 → 緑 LED）にする。
4. Burn を押し、ポートを選んで Start。

サーボの原点（home）は本体の NVS に保存されている。本ファームは原点を書き換えないが、ずれた場合は StackChan-BSP の `examples/Servo/HomeCalibration` で再設定できる。

## 音声素材の差し替え

`device/data/announce.wav`（セリフ）と `device/data/captured.wav`（撮影完了）は 16 kHz / mono / 16-bit の WAV。同じ形式のファイルを同名で置き換えてビルドし直せば差し替わる。仮音声は `device/tools/make_voice.sh` で macOS の TTS から生成している。

## 開発の流れ

- `main` に直接 push しない。`feat/...` ブランチで作業して PR を出す。
- PR テンプレートの実機チェックリストを埋めてからマージする。
- 鍵・Wi-Fi 情報は `config.h` / `config.toml` にだけ書く。`*.example` だけをコミットする。
- 顔写真の実サンプルは本人の同意なしにコミットしない。
