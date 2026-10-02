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
│   └── network.h/.cpp       # startNetwork の起動 (別タスクでブロックを避ける)、接続状態
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
- **Wi-Fi 起動**: `startNetwork` はブロックするので専用タスクで呼び、その間は待機画面に「Wi-Fi接続中」。未設定で繋がらない場合は「PC未接続」のまま診断画面へ行ける。純正の他アプリ（AI エージェント等）の Wi-Fi 利用を邪魔しない（切断しない）。
- **フレーム**: カメラ層が作る RGB565 LE（320×240、153,600 B）をそのまま `X-Format: rgb565` で送る。edge は `rgb565_byte_order = "little"`。首が動いている間と `HEAD_SETTLE_MS` 以内のフレームは送らない。
- **首**: edge の `servo_dx/dy` を `Head::nudge()` にそのまま渡す（クランプ・3°・500 ms は device 側で維持）。
- **音声**: ヒント `closer` で `closer.wav`（埋め込み済み）を 1 セッション 1 回。
- **ログ**: 鍵・URL・トークン・画像を出さない。送信 fps と往復時間を 5 秒ごとに出す。

## 4. 状態と画面（独立ファーム版 step2b §4.3/§4.4 と同じ。差分のみ）

- IDLE（待機画面）: 右下に「PC接続中」/「PC未接続」/「Wi-Fi接続中」。online でタッチ → 判定つき撮影。offline でタッチ → DIAG。
- DIAG: Wi-Fi 状態、edge host:port、最後のエラー。ボタン「再接続」「判定なしで撮影」。頭部タッチで待機へ戻る。
- PHOTO_QR: QR は `photo_url`、「削除予定 HH:MM」。X_QR: `share_url`。
- 「判定なしで撮影」は UPLOADING で ERROR「PC未接続のため保存できません」。

## 5. 受け入れチェック

- [ ] `idf.py build`（`config_local.h` あり / なしの両方）。
- [ ] 待機画面に「PC接続中」。edge を止めると 5〜10 秒で「PC未接続」、再起動で戻る。
- [ ] 顔を出すと 1 秒で CAPTURE。左右にずらすと首が同じ方向へ（逆なら edge の `[head] gain_x` の符号）。
- [ ] 1 人で笑うと「撮れたよ」→ 写真 QR をスマホのモバイル回線で開いて保存できる。X QR で投稿画面。
- [ ] 目を閉じる／笑わないと採用されず、時間切れで候補が出る。顔なしで「顔が見つからなかったよ」。
- [ ] アプリを閉じたあと純正アプリが正常。再度開いて撮影できる。
- [ ] 送信 fps ≥ 2。
