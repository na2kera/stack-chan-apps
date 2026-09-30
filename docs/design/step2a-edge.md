# ステップ2a 設計: edge を PC 単体で動かす

対象: `docs/spec.md` §11 の実装順 2 のうち、PC 側（`edge/`）。
device の Wi-Fi 接続と統合は 2b（`docs/design/step2b-device-edge.md`、後で書く）に回す。
2a の間、`device/` は触らない（別ブランチで音声の修正が進んでいるため）。

## 1. ゴールと非ゴール

ゴール:

- `docs/protocol.md` の全エンドポイントを持つ HTTP サーバーが `uv run edge` で起動する。
- MediaPipe Face Landmarker で複数顔（最大 4）を判定し、spec §6.2 の採用条件、§6.3 の首振り量、§6.4 のタイムアウト候補選びを実装する。
- device の代わりに **PC の webcam で同じフローを回す疑似デバイス**（`edge/tools/webcam_device.py`）があり、K151 なしで利用者が試せる。
- gallery はモック。edge 自身が `http://<host>:8765/mock/p/<token>` で写真ページを配り、TTL で消す。
- `uv run pytest` がハードウェアも MediaPipe のモデルも無しで通る（判定ロジックは偽の検出結果でテスト）。

非ゴール:

- device 側の変更（2b）。音声認識（ステップ3）。本物の gallery（ステップ4）。

## 2. 技術選定（固定値）

| 項目 | 値 | 根拠 |
| --- | --- | --- |
| Python | 3.11（uv 管理） | spec §3。`uv python install 3.11` |
| パッケージ管理 | uv（`pyproject.toml` + `uv.lock`） | 版固定と再現 |
| HTTP | FastAPI 0.142.2 + uvicorn 0.54.0 | spec §3 |
| 顔判定 | mediapipe 1.0.1（Tasks API `FaceLandmarker`、VIDEO モード、`num_faces=4`, `output_face_blendshapes=True`） | spec §3。**1.0 系で Tasks API が変わっていたら 0.10 系の最新に固定し直して報告する** |
| モデル | `face_landmarker.task`（float16）を `edge/models/` に置く（.gitignore） | `tools/download_model.py` で取得。URL: `https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/1/face_landmarker.task` |
| 画像 | numpy 2.4 + Pillow 12（RGB565 デコード、JPEG エンコード、ブレ計算） | OpenCV は mediapipe の依存で入るが、edge 本体では使わない（webcam ツールだけ使う） |
| テスト | pytest + httpx（FastAPI TestClient） | |
| lint | ruff | CI で `ruff check` と `pytest` |

## 3. ソース構成

```
edge/
├── pyproject.toml           # [project.scripts] edge = "edge.main:main"
├── uv.lock
├── config.example.toml      # コミット。実体 config.toml は .gitignore
├── README.md                # 起動・webcam での試し方・モデル取得
├── models/                  # .gitignore。face_landmarker.task
├── src/edge/
│   ├── main.py              # 引数と設定を読み、uvicorn を起動
│   ├── config.py            # TOML + 環境変数 (EDGE_DEVICE_KEY) → dataclass
│   ├── api.py               # FastAPI ルーティング。認証、JSON 変換だけ。ロジックは呼ぶだけ
│   ├── auth.py              # X-Device-Id / X-Device-Key の照合 (hmac.compare_digest)
│   ├── image.py             # RGB565 / JPEG → RGB ndarray、JPEG エンコード、ブレ (ラプラシアン分散)
│   ├── analysis.py          # FaceAnalyzer Protocol、FaceObservation、MediaPipeAnalyzer
│   ├── decision.py          # 純関数: 枠内判定、開眼・笑顔、目標人数、2 連続採用、タイムアウト時の順位づけ
│   ├── head.py              # 純関数: 外接矩形 → servo_dx/dy、探索、"closer" 判定
│   ├── session.py           # Session (状態、目標人数、保持フレーム、直近結果)、SessionStore (TTL 掃除)
│   ├── gallery.py           # Gallery Protocol、MockGallery (メモリ + TTL、/mock/p/<token> 用データ)
│   └── logging_setup.py     # 構造化ログ。トークン・画像バイトを出さない
├── tools/
│   ├── download_model.py
│   └── webcam_device.py     # 疑似デバイス (OpenCV で webcam → protocol.md の順に POST)
└── tests/
    ├── conftest.py          # FakeAnalyzer、MockGallery、TestClient
    ├── test_image.py
    ├── test_decision.py
    ├── test_head.py
    ├── test_session_flow.py # hello → start → compose frames → capture frames → accepted/timeout → review → photo
    └── test_auth.py
```

依存方向: `api → session → {analysis, decision, head, gallery, image}`。`decision.py` と `head.py` は numpy 以外に依存しない純関数にして、テストで検出結果を直接与える。

## 4. データ型

```python
@dataclass(frozen=True)
class FaceObservation:
    bbox: tuple[float, float, float, float]   # 正規化 (x0, y0, x1, y1)。ランドマークの min/max
    eye_blink_left: float                     # blendshape スコア 0..1
    eye_blink_right: float
    mouth_smile_left: float
    mouth_smile_right: float

@dataclass(frozen=True)
class FrameInput:
    session_id: str; frame_id: int; capture_ms: int
    servo_x: int; servo_y: int; width: int; height: int
    fmt: Literal["rgb565", "jpeg"]; phase: Literal["compose", "capture"]
    data: bytes

@dataclass(frozen=True)
class FrameVerdict:            # decision.py の出力。frame_result にそのまま写す
    face_count: int; target_face_count: int
    all_in_frame: bool; all_eyes_open: bool; all_smiling: bool
    hint: str | None; conditions_met: bool
```

`FaceAnalyzer` は `analyze(rgb: np.ndarray, timestamp_ms: int) -> list[FaceObservation]` だけを持つ Protocol。`MediaPipeAnalyzer` は起動時に 1 回モデルをロードする（spec §3）。VIDEO モードはタイムスタンプが単調増加である必要があるので、セッションをまたいでも増え続けるカウンタを使う。

## 5. 判定ルール（decision.py）

spec §6.2 の値はすべて `config.toml` の `[capture]` から渡す。初期値:

| 設定 | 初期値 |
| --- | --- |
| `max_faces` | 4 |
| `margin_ratio` | 0.08（上下左右）|
| `min_face_width_ratio` | 0.08 |
| `eye_blink_max` | 0.25（左右とも以下で開眼）|
| `mouth_smile_min` | 0.55（左右とも以上で笑顔）|
| `stable_frames` | 2（人数が連続で同じ）|
| `accept_consecutive` | 2（条件達成が連続したら 2 枚目を採用）|

- `in_frame(face)`: bbox 全体が margin の内側にあり、幅が `min_face_width_ratio` 以上。
- `all_eyes_open`, `all_smiling`: 全検出顔で判定。顔 0 は false。
- **目標人数**: COMPOSE 中は「連続 `stable_frames` フレームで同じ人数」だった最大値を記録し、COMPOSE の最後の値を目標にする。CAPTURE 中に安定して増えたら目標を増やす。減っても下げない。
- `conditions_met` = `face_count >= target` かつ `1 <= face_count <= max_faces` かつ全員 in_frame、開眼、笑顔。人数が前フレームと違う間は false（採用保留）。
- **採用**: `conditions_met` が `accept_consecutive` 回連続したフレームで `accepted=True`。そのフレームのバイト列（受信したままの RGB565/JPEG と、RGB にデコードしたもの）をセッションに保持する。以後のフレームは処理せず `accepted=False, dropped=True` で返す。
- `hint`: 顔数が `max_faces` を超える疑い（`face_count > max_faces`）なら `"too_many"`。外接矩形が margin を除いた幅・高さより大きいなら `"closer"`。
- **タイムアウト候補**（§6.4）: CAPTURE 中に受けたフレームごとに `(face_count, in_frame数, 開眼人数, 最小笑顔スコア, ブレ指標)` を記録し、この順の辞書順で最大のものを 1 枚だけバイト列ごと保持し続ける（メモリを 1 枚分に抑える）。顔 0 のフレームは候補にしない。`timeout` で `candidate` と理由文字列（例 `faces=2 in_frame=2 eyes_open=1 min_smile=0.31 sharpness=812`）を返し、ログにも残す。

## 6. 首振り（head.py）

spec §6.3。`[head]` 設定: `gain_x`, `gain_y`（px → 1/10 度。初期 -0.05 / 0.05、符号は実機で校正）、`deadband_px`（初期 16）、`step_max`（30）、`min_interval_ms`（500）、`search_step`（20）。

- 顔あり: 全顔の外接矩形の中心と画像中心の差 `(dx, dy)` px。`|dx| <= deadband` なら 0。それ以外は `clamp(dx * gain_x, ±step_max)`。顔の一部が margin を割っているときだけ、その方向へ寄せる補正を優先する。
- 顔なし（COMPOSE のみ）: `search_step` で左右交互に 1 段ずつ、往復は 2 回まで。CAPTURE 中は探索しない。
- 前回の指示から `min_interval_ms` 未満なら 0 を返す（device 側でも制限する二重の安全）。
- `servo_x` / `servo_y`（device が送る現在の目標角）が設定の可動域端にあり、まだ寄せたい方向が同じなら `hint="closer"` の材料にする（可動域は `[head]` に device と同じ値を書く）。

## 7. セッションと gallery

- `Session` は `state` (`compose` / `capture` / `timeout` / `review` / `uploading` / `done` / `cancelled`)、`target_face_count`、`last_frame_id`、`consecutive_met`、`accepted` (frame_id + bytes)、`best_candidate`、`photo` (url, share_url, expires_at) を持つ。
- `SessionStore` は dict + lock。5 分イベントの無いセッションを削除する。削除・retake・cancel で保持バイト列を消す。
- `review save`: 採用フレームか候補を JPEG（品質 90）にエンコードし、`Gallery.upload(jpeg, session_id, captured_at)` をバックグラウンドで呼ぶ。`GET /photo` で結果を返す。同じ session の save の再送は同じ結果を返す（冪等）。
- `MockGallery`: token は `secrets.token_urlsafe(16)`（128 ビット）。`/mock/p/{token}` で HTML（画像、ダウンロードボタン、削除予定時刻 JST、「URL を知る人は誰でも見られます」の注意）、`/mock/p/{token}.jpg` で JPEG。TTL（初期 60 分）を過ぎると 410。`/mock/share/x` は `https://x.com/intent/tweet?text=<URL エンコード>` へ 302。本文は `[share] text`（初期 `スタックチャンに撮ってもらいました！ #スタックチャン #StackChan`）。**モックは LAN 内向けで、QR を配る用途には使わない**旨をページに出す。

## 8. 疑似デバイス（tools/webcam_device.py）

利用者が K151 なしで試すための CLI。`uv run python tools/webcam_device.py --edge http://127.0.0.1:8765 --device-id … --key …`。

1. `hello` → 画面に "press SPACE to start" の webcam ウィンドウ。
2. SPACE で `session_start`。5 秒間 `phase=compose` で送る（約 5 fps、フレームは JPEG 品質 80、または `--format rgb565` で device と同じ形式）。
3. 10 秒間 `phase=capture`。ウィンドウに `faces / target / eyes / smile / servo_dx,dy / 残り秒` を重ねる。`accepted` が来たら "captured!" を出して止める。
4. 時間切れなら `timeout` を呼び、候補があれば表示して `s` で save / `r` で retake。
5. save 後は `GET /photo` をポーリングし、URL を標準出力に出して終了（`--open` でブラウザを開く）。

`servo_dx/dy` は webcam では動かせないので表示だけ。

## 9. 設定・ログ

`config.example.toml`:

```toml
[server]
host = "0.0.0.0"   # LAN 内の PC アドレスで listen。0.0.0.0 は開発時のみ
port = 8765

[auth]
device_id = "stackchan-01"
device_key = "change-me"   # 環境変数 EDGE_DEVICE_KEY が優先

[capture]
max_faces = 4
countdown_sec = 10
margin_ratio = 0.08
min_face_width_ratio = 0.08
eye_blink_max = 0.25
mouth_smile_min = 0.55
stable_frames = 2
accept_consecutive = 2
rgb565_byte_order = "little"

[head]
gain_x = -0.05
gain_y = 0.05
deadband_px = 16
step_max = 30
min_interval_ms = 500
search_step = 20
x_min = -250
x_max = 250
y_min = 250
y_max = 650

[analysis]
model_path = "models/face_landmarker.task"

[gallery]
mode = "mock"            # ステップ4で "http" を追加
ttl_minutes = 60
public_base_url = ""     # mock では http://<listen host>:<port> を使う

[share]
text = "スタックチャンに撮ってもらいました！ #スタックチャン #StackChan"
```

ログ（spec §9）: 1 行 1 イベントの JSON（`event`, `session_id`, `frame_id`, `state`, `face_count`, `target`, `latency_ms`, `servo_dx/dy`, `accepted`, `reason`）。写真バイト列・トークン・URL は出さない（`photo_ready` は `expires_at` だけ）。

## 10. テスト（ハードウェアもモデルも不要）

- `test_image.py`: RGB565 → RGB のデコード（既知の 2×2 ピクセル、両バイト順）、JPEG 往復、ブレ指標が鮮明画像 > ぼかし画像。
- `test_decision.py`: 各ルールを 1 つずつ。枠外、顔が小さい、片目閉じ、片方笑っていない、人数不安定、目標人数未満、2 連続で 2 枚目採用、5 人以上で不採用、タイムアウト順位づけ。
- `test_head.py`: デッドバンド、ゲインと符号、ステップ上限、間隔制限、探索の往復回数、"closer"。
- `test_session_flow.py`: FakeAnalyzer にフレームごとの観測列を与え、hello → start → compose → capture → accepted → review save → photo ready、および timeout → candidate → retake、cancel、古い frame_id の dropped、401。
- `test_gallery_mock.py`: token の長さ、TTL 後 410、share のリダイレクト先に写真 URL が含まれない。

実写サンプルはコミットしない。`MediaPipeAnalyzer` の実行はモデルファイルがあるときだけ動く手動テスト（`tests/manual_mediapipe.py`、pytest 対象外）にする。

## 11. 受け入れチェック（PR テンプレートに転記）

- [ ] `uv sync` → `uv run pytest` → `uv run ruff check` が通る。
- [ ] `uv run python tools/download_model.py` でモデルが取れ、`uv run edge` が起動して `POST /v1/hello` に 200 を返す。鍵が違うと 401。
- [ ] `tools/webcam_device.py` で 1 人: 目を開けて笑うと 2 フレーム連続で `accepted`。目を閉じる、または笑わないと採用されない。
- [ ] 2 人で試し、1 人が笑わないと採用されない。途中で 1 人が画角から外れると人数が減っても採用されない（目標人数の維持）。
- [ ] 顔を左右にずらすと `servo_dx` の符号が変わり、|値| が 30 を超えない。
- [ ] 10 秒で時間切れになり、候補と理由がログに出る。save でモックの写真ページが開き、TTL を短く設定すると 410 になる。
- [ ] ログに画像バイト列・トークン・URL が出ない。
