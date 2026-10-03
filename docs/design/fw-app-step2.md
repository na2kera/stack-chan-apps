# 純正ファーム内アプリ ステップ2 設計: app_photobooth を edge / gallery に繋ぐ

対象: `docs/spec.md` §11 の実装順 2 と 4 の device 側。独立ファーム版の `docs/design/step2b-device-edge.md` を純正ファーム内アプリ (`firmware/main/apps/app_photobooth/`) に移植する。
通信契約は `docs/protocol.md`（変更なし）。edge と gallery は実装済み（PR #3, #7）で、このステップでは触らない。

## 1. ゴール

- アプリを開くと純正の Wi-Fi 設定（NVS）で接続し、edge に `hello` して待機画面に「PC接続中」/「PC未接続」を出す。
- COMPOSE / CAPTURE でフレーム（QVGA RGB565 little endian）を edge に送り、`servo_dx/dy` で首を寄せ、人数/目標とヒント（「もう少し寄ってね」「4人までだよ」）を出す。
- `accepted` で「撮れたよ」→ `review save` → `photo` で受けた **本物の URL** を PHOTO_QR / X_QR に出し、削除時刻（`expires_at` の HH:MM）を表示する。
- 時間切れなら `timeout` → edge の候補 JPEG を REVIEW に表示。顔なしは「顔が見つからなかったよ」。
- edge に繋がらないときは自動判定つきの撮影を始めず、診断画面（Wi-Fi 状態 / edge host:port / 最後のエラー、「再接続」「判定なしで撮影」）を出す（spec §9）。
- 設定ファイルが無い（edge 未設定）ビルドでは、ステップ1 と同じ単体動作（固定 URL）になる。

## 2. 技術選定

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| Wi-Fi | 純正 `GetHAL().startNetwork(onLog)`（NVS の設定で接続。`app_app_center` と同じ使い方）と `getWifiStatus()` | SSID/パスワードをコードに書かない |
| HTTP | ESP-IDF `esp_http_client`（keep-alive、タイムアウト 3 秒） | 追加依存なし |
| JSON | `components/ArduinoJson`（v7.4.2、純正が取得済み） | 独立ファーム版と同じ |
| 候補 JPEG | 純正 `hal/utils/jpeg_to_image/jpeg_decoder.h` の `jpeg_dec::decode_to_lvgl` | `hal_ws_avatar.cpp` と同じ |
| 名前解決 | `EDGE_HOST` は IP アドレス。`.local` 名 (mDNS) は依存の追加が要るので今回は入れない (提案として README に書く) | PC の IP は DHCP で変わる (実際に 192.168.0.167 → .35 に変わった) ので、診断画面に接続先を出す |
| 設定 | `app_photobooth/config_local.h`（.gitignore）。`config_local.example.h` をコミット。`__has_include` で無ければ edge 無効 | 鍵を Git に入れない |

## 3. 構成（追加・変更）

```
app_photobooth/
├── config_local.example.h   # EDGE_HOST, EDGE_PORT, DEVICE_ID, EDGE_SHARED_KEY
├── net/
│   ├── edge_client.h        # 抽象 IF (独立ファーム版 device/src/edge/edge_client.h の最終形を移植)
│   ├── null_edge.h          # 常に offline
│   ├── http_edge_client.h/.cpp  # net タスク + コマンドキュー + 2 面フレームスロット + mailbox
│   └── network.h/.cpp       # NVS に Wi-Fi 設定があるか、接続状態、診断用の情報
├── flow/flow.cpp            # edge の判定で COMPOSE/CAPTURE/REVIEW/UPLOADING を駆動、DIAG 状態を追加
└── view/                    # 接続表示、案内帯、人数/目標、診断画面、候補 JPEG、削除時刻
```

- 移植元は独立ファーム版の最終コード（ブランチ `origin/feat/device-step2b` の `device/src/net/http_edge_client.{h,cpp}`、`device/src/app/app.{h,cpp}`、`device/src/edge/*.h`、`device/src/ui/*`）。レビューで直した点をすべて引き継ぐ:
  - `accepted` は別枠で保持し、後続の `dropped` 応答で上書きしない。accepted 後はそのセッションでフレームを送らない。
  - `timeout` は成功/失敗を区別（200 + JSON のみ成功）。失敗は ERROR「PCとの接続が切れました」。
  - online の時刻は 2xx でだけ進める。3 回連続失敗で offline。hello は 5 秒間隔。
  - save の送信は session ごとに合計 3 回まで。
  - フレームは再送しない。古い未送信フレームは捨てる（newest wins）。
  - 10 秒経過後に届いた `accepted` は無視。首を動かす前に撮ったフレームへの `servo_dx/dy` は捨てる。
- **タスクの寿命**: fw-app ステップ1 のカメラ/音声と同じ worker + shared_ptr 方式。アプリを閉じるとき net タスクが 1 秒で止まらなければ切り離し、タスクが自分で後始末する。切り離し中は再接続を断る。
- **LVGL**: 画面操作は `View` の中で `LvglLockGuard`。net タスクから LVGL/HAL を呼ばない。部品は `Page::add<T>()` で持つ（型を落として delete しない。fw-app ステップ1 の再起動バグの原因）。
- **Wi-Fi 起動**: 純正 App Center と同じく、アプリを開いたときに「Wi-Fi接続中」の画面を出して `startNetwork` を同期で呼ぶ（NVS に SSID があり、未接続のときだけ）。専用タスクは持たない（純正アプリの `startNetwork` と Board のコールバックを取り合うため。レビュー round 1）。SSID があるのに繋がらない場合は純正の動作（約 60 秒で設定 AP）になる。未設定なら呼ばず、「PC未接続」のまま診断画面へ行ける。アプリを閉じても Wi-Fi は切らない。
- **フレーム**: カメラ層が作る RGB565 LE（320×240、153,600 B）をそのまま `X-Format: rgb565` で送る。edge は `rgb565_byte_order = "little"`。首が動いている間と `HEAD_SETTLE_MS` 以内のフレームは送らない。
- **首**: edge の `servo_dx/dy` を `Head::nudge()` にそのまま渡す（クランプ・3°・500 ms は device 側で維持）。
- **音声**: ヒント `closer` で `closer.wav`（埋め込み済み）を 1 セッション 1 回。
- **ログ**: 鍵・URL・トークン・画像を出さない。送信 fps と往復時間を 5 秒ごとに出す。

## 4. 状態と画面（独立ファーム版 step2b §4.3/§4.4 と同じ。差分のみ）

- IDLE（待機画面）: 右下に「PC接続中」/「PC未接続」。online でタッチ → 判定つき撮影。offline でタッチ → DIAG。
- DIAG: Wi-Fi 状態、edge host:port、最後のエラー。ボタン「再接続」「判定なしで撮影」。頭部タッチで待機へ戻る。
- PHOTO_QR: QR は `photo_url`、「削除予定 HH:MM」。X_QR: `share_url`。
- 「判定なしで撮影」は UPLOADING で ERROR「PC未接続のため保存できません」。

### シャッター演出（SHUTTER）

edge の判定で自動採用されたときだけ、CAPTURE と UPLOADING の間に SHUTTER を挟む。REVIEW の「保存する」と「判定なしで撮影」の経路は変えない。

```
CAPTURE --(frame_result accepted)--> SHUTTER --(表示 ≥ CAPTURED_HOLD_MS かつ captured.wav 終了)--> UPLOADING
```

- 採用時（CAPTURE 内）: 候補 JPEG を依頼（`requestCandidate`）→ `review save` の順に送る。edge は公開が終わるとフレームを捨てる（candidate が 404）ので、候補を先に取りに行く。
- 0 〜 `SHUTTER_FLASH_MS`（150 ms）: 全面を白にする（フラッシュ）。同時に `shutter.wav`（合成音 240 ms、`tools/make_shutter.py` で生成）を鳴らす。
- フラッシュの後: 撮れた写真を全面に止めて表示し、タイトル帯「撮れたよ」を重ねる（ボタンなし）。
  - 写真は edge の候補 JPEG（採用フレーム。REVIEW と同じ `jpeg_info` の検査とデコード）。フラッシュの終わりまでに届かない・取れない・デコードできないときは、CAPTURE で最後に描いたプレビューを止めたまま出す。表示中に候補が届いたら差し替える（表示時間は延ばさない）。
- `shutter.wav` が終わったら `captured.wav`（「撮れたよ」）。写真の表示から `CAPTURED_HOLD_MS`（1000 ms）以上たち、かつ `captured.wav` が終わったら UPLOADING へ。UPLOADING は従来どおり「撮れたよ／写真を準備中」を出して `photo_ready` を待つ（`captured.wav` は鳴らし直さない）。
- SHUTTER の間は frame_result を使わず（読み捨て）、CAPTURE の 10 秒の期限も見ない。取り込みは止める。
- SHUTTER を抜けるまでに候補が届かなければ、届くか `EdgeClient` が諦めるまで受け取って捨てる。アプリを閉じたら他の状態と同じ後始末（`sessionCancel`、View が持つデコード結果の解放）。
- ログ: SHUTTER への遷移、候補／プレビューのどちらを出したか、フラッシュ・音・表示の各時間。URL・鍵・画像は出さない。

## 4.5 レビュー round 1 で決めたこと

- 判定つきの REVIEW は edge の候補 JPEG だけを表示する。取得・デコードできない、または 320x240 でないときは「候補の写真を表示できません」と「撮り直す」だけ（表示と保存対象をずらさない）。
- リクエストは開始時に `EDGE_TIMEOUT_MS` (3 秒) の期限を決め、ブロックする呼び出しの前に残り時間を設定する。`esp_http_client` の内部ループは 1 回の送受信ごとに時間を見るため、応答を少しずつ返す相手では期限を超え得る (LAN 内の edge では許容。番人による強制切断は、タイマータスクのブロックと fd の取り違えのリスクが上回るため採用しない)。
- chunked 応答は上限（JSON 2 KB、JPEG 256 KB）まで読む。
- `photo_ready` は `photo_url` / `share_url` / `expires_at` がすべて非空のときだけ成功。QR を生成できなければ ERROR「QRを表示できません」。
- フレーム専用ヘッダは毎リクエスト消してから付け直す。

## 5. 受け入れチェック

- [ ] `idf.py build`（`config_local.h` あり / なしの両方）。
- [ ] 待機画面に「PC接続中」。edge を止めると 5〜10 秒で「PC未接続」、再起動で戻る。
- [ ] 顔を出すと 1 秒で CAPTURE。左右にずらすと首が同じ方向へ（逆なら edge の `[head] gain_x` の符号）。
- [ ] 1 人で笑うとシャッター音と白フラッシュ → 撮れた写真に「撮れたよ」（1 秒以上、声が終わるまで）→ 写真 QR をスマホのモバイル回線で開いて保存できる。X QR で投稿画面。
- [ ] 目を閉じる／笑わないと採用されず、時間切れで候補が出る。顔なしで「顔が見つからなかったよ」。
- [ ] アプリを閉じたあと純正アプリが正常。再度開いて撮影できる。
- [ ] 送信 fps ≥ 2。
