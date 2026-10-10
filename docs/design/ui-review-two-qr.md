# 画面変更: 確認 →「次へ」→ QR 2 つを 1 画面に

対象: `firmware/main/apps/app_photobooth/` の REVIEW 以降の画面 (ユーザー指示、2026-10-10)。通信・判定のロジックは変えない。

## 1. 変更の要約

| 今 | 変更後 |
| --- | --- |
| SHUTTER (撮れたよ、1 秒) → REVIEW (候補写真 +「保存する」「撮り直す」) → UPLOADING → PHOTO_QR (写真 QR +「次へ」「撮り直す」) → X_QR (X QR + 案内 +「戻る」「終了」) | SHUTTER → REVIEW (候補写真 +「撮り直す」「次へ」) → UPLOADING → **QR (写真 QR と X QR を左右に並べる +「撮り直す」「終了」)** |

- REVIEW の「保存する」を「次へ」に改名する (動作は同じ: 保存 = アップロードに進む)。ボタンの並びは左「撮り直す」、右「次へ」。
- PHOTO_QR と X_QR を 1 つの状態 `State::Qr` に統合する。X_QR の案内文「保存した写真をXに添付してね」は消す (ユーザー指示)。
- 「戻る」「次へ」は QR 画面から無くなる。残るボタンは左「撮り直す」、右「終了」。
- 判定なし撮影 (edge なし / 固定 URL) も同じ QR 画面を使う (固定 URL 2 つ)。

## 2. QR 画面のレイアウト (320×240)

```
y=0   ┌────────────────────────────────────┐
      │ タイトル帯 (kTitleH=32): 「写真を保存」   │   ← タイトルは既存の帯を使う (下の見出しと重ねない)
y=32  │  写真を保存            Xに投稿        │   ← 見出し 2 つ (各 QR の上、幅 = QR と同じ、高さ 24)
y=56  │ ┌──────────┐      ┌──────────┐       │
      │ │  QR 写真  │      │  QR  X   │       │   ← QR 2 つ。size = 132、x = 20 と 168 (間 16)、y = 56..188
y=188 │ └──────────┘      └──────────┘       │
      │ 削除予定 14:52 (左 QR の下に収まらなければ見出しの行の右端など、実装で決めてよい) │
y=192 │ [撮り直す]            [終了]           │   ← ボタン帯 (kButtonBarH=48)
y=240 └────────────────────────────────────┘
```

- QR の描画は既存の `view::qr()` (`view/widgets.cpp`) をそのまま 2 回呼ぶ。`lv_qrcode` は ECC M で入る最小 version を選び、1 モジュール = floor(size / モジュール数) px で描く。写真 URL (`https://stackchan-gallery.na2kera.workers.dev/p/<token>`、70〜80 文字、version 4〜5 = 33〜37 モジュール) は size 132 で 3 px/モジュール (99〜111 px)、X の URL (`…/share/x`、約 50 文字、version 3 = 29 モジュール) は 4 px (116 px)。
- 既存の定数 `kQrSize/kQrX/kQrY/kQrTextX/kQrTextW` (`view/widgets.h:32-37`) は使われなくなるので、新しい定数に置き換える (`kQrPairSize = 132`、`kQrLeftX = 20`、`kQrRightX = 168`、`kQrPairY = 56`、見出し `Rect`)。`static_assert` でボタン帯と重ならないことを保つ。
- 「削除予定 HH:MM」は残す (写真の期限は利用者に要る)。置き場所は、見出し行の高さを 24 にして QR を y=56 から 132 にすると y=188 で帯の直前になるため、**見出し行の右端 (X の見出しの右) ではなく、タイトル帯の右側** (タイトル「写真を保存」の右に `削除予定 14:52` を muted 色で) に置く。タイトル帯の幅に収まらなければ「14:52まで」の短縮も可。色・フォントは既存のものだけを使う。
- 色・フォント・ボタン帯・タイトル帯の見た目は変えない。

## 3. ソース構成

```
firmware/main/apps/app_photobooth/
├── flow/flow.cpp, flow.h      # State::PhotoQr / XQr → State::Qr。kBtnPhotoNext/kBtnPhotoRetake/kBtnXBack/kBtnXExit → kBtnQrRetake=0 / kBtnQrExit=1。REVIEW のボタン index は 左 撮り直す=0、右 次へ=1 に入れ替え
├── view/view.h, view.cpp      # showPhotoQr/showXQr → showQr(title, photo_url, share_url, expires_at)。showReview のボタン並びを 撮り直す / 次へ に
├── view/widgets.h             # QR の定数
├── view/strings.h             # kTitlePhotoQr「写真を保存」は QR 画面のタイトルに流用、kTitleXQr「Xに投稿」は右の見出しに流用、kXQrGuide を削除、kBtnNext は REVIEW で使う、kBtnBack は未使用なら削除
└── (設計書) docs/design/fw-app-step1.md / fw-app-step2.md の画面遷移表に「変更 (ui-review-two-qr.md)」の注記
```

`docs/spec.md` §画面 (X_QR の行) も「QR 画面 (2 つ)」に更新する。

## 4. 試験

- ホストテスト: Flow の状態遷移がロジック層にあればそれを更新 (無ければ追加しない。view は実機)。
- 実機: 撮影一周 → QR 画面に 2 つ出る → スマホで左 (写真ページが開く) と右 (X の投稿画面が開く) をそれぞれ読める → 「撮り直す」で新しい撮影、「終了」でアプリ終了 (純正ホームに戻る) → 判定なし撮影 (edge なし) でも QR 2 つ (固定 URL)。
- 読み取れない場合の次の手: QR のサイズを 140 に広げる (x = 12 / 168)、または一方をタップで拡大 (このステップではやらない)。
