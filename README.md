# stack-chan-photobooth

M5Stack StackChan K151 に「写真撮りたい」と話しかける（またはタッチする）と、首を動かして全員を画角に収め、全員が目を開けて笑った瞬間の写真を撮り、ダウンロード用 QR と X 投稿用 QR を表示する自動撮影アプリ。

仕様は [docs/spec.md](docs/spec.md)、設計は [docs/design/](docs/design/) を参照。

## 構成

```
device/    K151 に書き込む独立ファーム (PlatformIO, Arduino-ESP32)
edge/      PC で動かす Python サービス (音声認識・顔判定・首振り計算)   ※ステップ2以降
gallery/   公開 HTTPS の写真配布サービス                                 ※ステップ4以降
docs/      仕様・設計・通信契約
```

現在の進捗: ステップ1（device 単体でフローを通す）を実装中。

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
