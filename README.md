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

現在の進捗: ステップ2b（device を Wi-Fi で edge に繋ぎ、顔判定と首振りを実機で動かす）を実装中。

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

edge (PC) を使わずにステップ1と同じ固定 QR のフローで動かすときは、`build_flags` の `-DPHOTOBOOTH_NO_EDGE` のコメントを外してビルドする（Wi-Fi に繋がず、`NullEdge` を使う）。

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

## edge (PC) と繋ぐ（ステップ2b）

device は同じ LAN の PC で動く `edge` に HTTP でフレームを送り、顔判定・首振り量・写真の保存を任せる。通信契約は [docs/protocol.md](docs/protocol.md)。

1. **Wi-Fi**: `device/include/config.h` の `WIFI_SSID` / `WIFI_PASSWORD` を設定する。K151 (ESP32-S3) は 2.4 GHz だけに対応する。
2. **PC のアドレス**: PC (macOS) で次を実行し、出たアドレスを `config.h` の `EDGE_HOST` に書く（Wi-Fi が `en0` でない Mac では `en1` など）。

   ```console
   ipconfig getifaddr en0
   ```

3. **鍵と ID を揃える**: `config.h` の `DEVICE_ID` / `EDGE_SHARED_KEY` と、edge の `config.toml` の `[auth] device_id` / `device_key`（または環境変数 `EDGE_DEVICE_KEY`）を同じ値にする。違うと edge が 401 を返し、device の診断画面に「認証エラー」と出る。鍵は `config.h` / `config.toml` にだけ書き、コミットしない。
4. **edge を LAN 向けに起動する**: `127.0.0.1` で listen すると K151 から届かない。PC のアドレスか `0.0.0.0` を指定する（詳細は [edge/README.md](edge/README.md)）。

   ```console
   cd edge
   EDGE_DEVICE_KEY=<config.h と同じ鍵> uv run edge --host 0.0.0.0   # または --host <ipconfig getifaddr en0 の値>
   ```

   macOS のファイアウォールが有効なら、初回に Python の受信接続を許可する。
5. device を書き込んで起動すると「Wi-Fi接続中」（最大 10 秒）のあと IDLE になり、右下に「PC接続中」が出る。「PC未接続」のままならタッチで診断画面を開き、SSID / IP / RSSI / edge の host:port / 最後のエラーを確認する。「再接続」で Wi-Fi から繋ぎ直し、「判定なしで撮影」で edge なしの撮影（保存はできない）を試せる。頭をタッチすると IDLE に戻る。

シリアルログ (`pio device monitor`) には送信 fps と往復時間（`net: send N frames ... fps, rtt avg ...`）が 5 秒ごとに出る。鍵・URL・画像はログに出さない。

## 起動と操作

1. 起動すると顔と「写真を撮りたい、と言ってね」が出る。
2. 「PC接続中」のときに画面か頭の上をタッチすると「写真を撮るよ！ いい顔をしてね」と話し、構図あわせ（全員が枠に入って 1 秒、最長 5 秒。首が顔の方へ寄る）のあと 10 秒のカウントが始まる。
3. 全員が目を開けて笑うと「撮れたよ」と言って写真 QR が出る。10 秒で撮れなければ edge が選んだ候補写真と「保存する」「撮り直す」が出る。
4. 写真 QR の「次へ」で X 投稿 QR が出る。QR の中身は edge が返す URL（ステップ2b では edge のモック写真ページ。同じ Wi-Fi のスマホで開ける）。削除予定時刻も edge の値。

`-DPHOTOBOOTH_NO_EDGE` でビルドしたときはステップ1と同じく、タッチで固定フロー（首を左右に少し動かす → 10 秒 → 候補 → `config.h` の固定 URL の QR）になる。

## 復旧（純正ファームに戻す）

M5Stack 公式の M5Burner で StackChan の純正ファームを書き戻せる。

1. [M5Burner (macOS)](https://m5burner-cdn.m5stack.com/app/M5Burner-v3-mac-x64.dmg) を入れる（Windows / Linux は [公式ドキュメント](https://docs.m5stack.com/en/StackChan) 参照）。
2. M5Burner を開き、「Only Official」にチェックして `StackChan` を検索し、最新の純正ファームをダウンロードする。
3. K151 を USB-C で接続して電源を入れ、必要ならダウンロードモード（リセット長押し 2 秒 → 緑 LED）にする。
4. Burn を押し、ポートを選んで Start。

サーボの原点（home）は本体の NVS に保存されている。本ファームは原点を書き換えないが、ずれた場合は StackChan-BSP の `examples/Servo/HomeCalibration` で再設定できる。

## 音声素材の差し替え

`device/data/announce.wav`（セリフ）、`device/data/captured.wav`（撮影完了）、`device/data/closer.wav`（「もう少し寄ってね」）は 16 kHz / mono / 16-bit の WAV。同じ形式のファイルを同名で置き換えてビルドし直せば差し替わる。仮音声は `device/tools/make_voice.sh` で macOS の TTS から生成している。

## 開発の流れ

- `main` に直接 push しない。`feat/...` ブランチで作業して PR を出す。
- PR テンプレートの実機チェックリストを埋めてからマージする。
- 鍵・Wi-Fi 情報は `config.h` / `config.toml` にだけ書く。`*.example` だけをコミットする。
- 顔写真の実サンプルは本人の同意なしにコミットしない。
