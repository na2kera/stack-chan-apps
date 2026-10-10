"""疑似デバイス: PC の webcam で K151 と同じ HTTP フロー (docs/protocol.md) を回す (design §8)。

使い方:
    uv run python tools/webcam_device.py --edge http://127.0.0.1:8765 \
        --device-id stackchan-01 --key <EDGE_DEVICE_KEY> [--format rgb565] [--open]

操作: SPACE で撮影開始 / 候補表示中は s で保存・r で撮り直し / q か ESC で終了。
servo_dx / servo_dy は webcam では動かせないので表示だけ。
edge 本体では OpenCV を使わない。cv2 を import するのはこのツールだけ。

計測 (docs/design/step6-cloud-device.md §3.4):
    --stats out.json          終了時に件数つきの p50 / p95 / max を JSON で書く
    --candidate-via-edge      候補の表示に edge の GET …/candidate を使う (device と同じ)
    --wait-ready-sec 120      起動時の hello を 200 が返るまで繰り返す (cold start の計測)
    --headless --auto-capture --duration-sec 600
                              ウィンドウを出さず、撮影開始 → 候補取得 → 撮り直しを自動で回す。
                              5 秒ごとの hello (heartbeat) も送る。保存は --auto-save のときだけ
"""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import os
import sys
import time
import uuid
import webbrowser
from collections import Counter
from collections.abc import Callable
from dataclasses import dataclass, field
from datetime import UTC, datetime
from typing import Any

import httpx
import numpy as np

from edge.image import encode_rgb565

try:
    import cv2
except ImportError:  # CI などで GUI ライブラリが無いとき。判定部分のテストだけは読めるようにする
    cv2 = None  # type: ignore[assignment]

SERVO_X_NEUTRAL = 0  # device の正面姿勢 (config.example.h と同じ)
SERVO_Y_NEUTRAL = 450
KEY_SPACE = 32
KEY_ESC = 27
WINDOW = "stackchan webcam device"
GREEN = (80, 220, 80)
WHITE = (255, 255, 255)
YELLOW = (0, 220, 255)
RED = (60, 60, 255)
HELLO_TIMEOUT_SEC = (
    8.0  # cold start 待ちの hello だけ長くする (docs/design/step6-cloud-device.md §3.2)
)
HELLO_RETRY_SEC = 2.0


class Quit(Exception):
    pass


class EdgeHTTPError(RuntimeError):
    """edge (または Worker) が 4xx / 5xx を返した。"""

    def __init__(self, method: str, path: str, status: int, text: str) -> None:
        super().__init__(f"{method} {path} -> {status} {text}")
        self.status = status
        try:
            self.code = str(json.loads(text).get("error", ""))
        except (ValueError, AttributeError):
            self.code = ""


class SessionLost(Exception):
    """セッションが edge から消えた (コンテナの再起動など)。device なら ERROR → 撮り直し。"""


@dataclass
class Options:
    fmt: str
    byte_order: str
    width: int
    height: int
    fps: float
    compose_sec: float
    capture_sec: float
    open_browser: bool
    candidate_via_edge: bool = False
    headless: bool = False
    # 自動モードだけ True。unknown_session で撮影を打ち切る (対話モードの挙動は変えない)
    abort_on_lost: bool = False


# ---- 計測 ----


def percentile(values: list[float], q: float) -> float:
    """最近傍順位法の百分位 (件数が少なくても実在する値を返す)。"""
    ordered = sorted(values)
    rank = max(1, math.ceil(q / 100.0 * len(ordered)))
    return ordered[rank - 1]


def summarize(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"count": 0}
    return {
        "count": len(values),
        "p50": round(percentile(values, 50), 1),
        "p95": round(percentile(values, 95), 1),
        "max": round(max(values), 1),
        "mean": round(sum(values) / len(values), 1),
    }


def error_key(exc: BaseException) -> str:
    if isinstance(exc, EdgeHTTPError):
        return f"{exc.status}:{exc.code}" if exc.code else str(exc.status)
    return type(exc).__name__


def body_framing(headers: httpx.Headers) -> str:
    """応答本文が Content-Length 付きか chunked か (ファームの HTTP クライアントで効く)。"""
    if "chunked" in headers.get("transfer-encoding", "").lower():
        return "chunked"
    if "content-length" in headers:
        return "content-length"
    return "none"


@dataclass
class FrameStats:
    attempted: int = 0
    ok: int = 0
    rtt_ms: list[float] = field(default_factory=list)
    latency_ms: list[float] = field(default_factory=list)
    bytes: list[float] = field(default_factory=list)
    interval_ms: list[float] = field(default_factory=list)  # 同じフェーズ内の送信開始の間隔
    loop_sec: float = 0.0  # フレームを回していた時間の合計 (実効 fps の分母)
    errors: Counter[str] = field(default_factory=Counter)


@dataclass
class Stats:
    """--stats の集計。鍵・URL・画像は持たない。"""

    started_at: str = field(default_factory=lambda: datetime.now(UTC).isoformat())
    cold_hello: dict[str, Any] | None = None
    hello_rtt_ms: list[float] = field(default_factory=list)
    hello_errors: Counter[str] = field(default_factory=Counter)
    frames: dict[str, FrameStats] = field(default_factory=dict)
    candidate_rtt_ms: list[float] = field(default_factory=list)
    candidate_bytes: list[float] = field(default_factory=list)
    candidate_framing: Counter[str] = field(default_factory=Counter)
    candidate_errors: Counter[str] = field(default_factory=Counter)
    photo_ready_ms: list[float] = field(default_factory=list)
    save_errors: Counter[str] = field(default_factory=Counter)
    sessions: Counter[str] = field(default_factory=Counter)
    events: list[dict[str, Any]] = field(default_factory=list)  # セッション喪失などの時刻

    def frame(self, fmt: str) -> FrameStats:
        return self.frames.setdefault(fmt, FrameStats())

    def event(self, name: str, **fields: Any) -> None:
        self.events.append({"at": datetime.now(UTC).isoformat(), "event": name, **fields})

    def to_json(self, meta: dict[str, Any] | None = None) -> dict[str, Any]:
        frames: dict[str, Any] = {}
        for fmt, f in self.frames.items():
            p50 = percentile(f.interval_ms, 50) if f.interval_ms else 0.0
            p95 = percentile(f.interval_ms, 95) if f.interval_ms else 0.0
            frames[fmt] = {
                "attempted": f.attempted,
                "ok": f.ok,
                "failure_rate": round(1 - f.ok / f.attempted, 4) if f.attempted else None,
                "rtt_ms": summarize(f.rtt_ms),
                "latency_ms": summarize(f.latency_ms),
                "bytes": summarize(f.bytes),
                "interval_ms": summarize(f.interval_ms),
                "fps": {
                    "count": len(f.interval_ms),
                    # 間隔の p50 / p95 を fps に直したもの (p95 は遅い側)
                    "p50": round(1000.0 / p50, 2) if p50 else None,
                    "p95_slow": round(1000.0 / p95, 2) if p95 else None,
                    "effective": round(f.ok / f.loop_sec, 2) if f.loop_sec else None,
                },
                "errors": dict(f.errors),
            }
        return {
            "meta": {"started_at": self.started_at, **(meta or {})},
            "cold_hello": self.cold_hello,
            "hello": {"rtt_ms": summarize(self.hello_rtt_ms), "errors": dict(self.hello_errors)},
            "frames": frames,
            "candidate": {
                "rtt_ms": summarize(self.candidate_rtt_ms),
                "bytes": summarize(self.candidate_bytes),
                "framing": dict(self.candidate_framing),
                "errors": dict(self.candidate_errors),
            },
            "save": {
                "photo_ready_ms": summarize(self.photo_ready_ms),
                "errors": dict(self.save_errors),
            },
            "sessions": dict(self.sessions),
            "events": self.events,
        }


def _ms_since(t0: float) -> float:
    return (time.monotonic() - t0) * 1000.0


class EdgeClient:
    def __init__(
        self,
        base_url: str,
        device_id: str,
        key: str,
        stats: Stats | None = None,
        transport: httpx.BaseTransport | None = None,
    ) -> None:
        self.device_id = device_id
        self.stats = stats
        self.http = httpx.Client(
            base_url=base_url.rstrip("/"),
            headers={"X-Device-Id": device_id, "X-Device-Key": key},
            timeout=3.0,  # device と同じ 3 秒
            transport=transport,
        )
        self.last_hello = 0.0

    def _json(self, r: httpx.Response) -> dict[str, Any]:
        if r.status_code >= 400:
            raise EdgeHTTPError(r.request.method, r.request.url.path, r.status_code, r.text)
        return r.json()

    def hello(self, timeout: float | None = None) -> dict[str, Any]:
        body = {"device_id": self.device_id, "protocol_version": 1}
        t0 = time.monotonic()
        self.last_hello = t0
        kwargs: dict[str, Any] = {} if timeout is None else {"timeout": timeout}
        try:
            info = self._json(self.http.post("/v1/hello", json=body, **kwargs))
        except (httpx.HTTPError, RuntimeError) as exc:
            if self.stats:
                self.stats.hello_errors[error_key(exc)] += 1
            raise
        if self.stats:
            self.stats.hello_rtt_ms.append(_ms_since(t0))
        return info

    def heartbeat(self, every_sec: float) -> None:
        """device と同じく every_sec ごとに hello を送る (warm を保つ)。失敗は数えるだけ。"""
        if every_sec <= 0 or time.monotonic() - self.last_hello < every_sec:
            return
        with contextlib.suppress(httpx.HTTPError, RuntimeError):
            self.hello()

    def start(self, sid: str) -> None:
        self._json(self.http.post("/v1/sessions", json={"session_id": sid, "started_at_ms": 0}))

    def frame(
        self, sid: str, frame_id: int, capture_ms: int, rgb: np.ndarray, opt: Options, phase: str
    ) -> dict[str, Any]:
        if opt.fmt == "rgb565":
            body = encode_rgb565(rgb, opt.byte_order)  # type: ignore[arg-type]
        else:
            ok, buf = cv2.imencode(
                ".jpg", cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR), [cv2.IMWRITE_JPEG_QUALITY, 80]
            )
            if not ok:
                raise RuntimeError("jpeg encode failed")
            body = buf.tobytes()
        headers = {
            "Content-Type": "application/octet-stream",
            "X-Frame-Id": str(frame_id),
            "X-Capture-Ms": str(capture_ms),
            "X-Servo-X": str(SERVO_X_NEUTRAL),
            "X-Servo-Y": str(SERVO_Y_NEUTRAL),
            "X-Width": str(rgb.shape[1]),
            "X-Height": str(rgb.shape[0]),
            "X-Format": opt.fmt,
            "X-Phase": phase,
        }
        fs = self.stats.frame(opt.fmt) if self.stats else None
        t0 = time.monotonic()
        if fs:
            fs.attempted += 1
            fs.bytes.append(float(len(body)))
        try:
            res = self._json(
                self.http.post(f"/v1/sessions/{sid}/frames", content=body, headers=headers)
            )
        except (httpx.HTTPError, RuntimeError) as exc:
            if fs:
                fs.errors[error_key(exc)] += 1
            raise
        if fs:
            fs.ok += 1
            fs.rtt_ms.append(_ms_since(t0))
            if isinstance(res.get("latency_ms"), (int, float)):
                fs.latency_ms.append(float(res["latency_ms"]))
        return res

    def timeout(self, sid: str) -> dict[str, Any]:
        return self._json(self.http.post(f"/v1/sessions/{sid}/timeout"))

    def review(self, sid: str, decision: str) -> dict[str, Any]:
        return self._json(self.http.post(f"/v1/sessions/{sid}/review", json={"decision": decision}))

    def candidate(self, sid: str) -> bytes:
        """REVIEW 表示用の候補 JPEG (device と同じ GET …/candidate)。"""
        t0 = time.monotonic()
        try:
            r = self.http.get(f"/v1/sessions/{sid}/candidate")
            if r.status_code >= 400:
                raise EdgeHTTPError("GET", r.request.url.path, r.status_code, r.text)
        except (httpx.HTTPError, RuntimeError) as exc:
            if self.stats:
                self.stats.candidate_errors[error_key(exc)] += 1
            raise
        if self.stats:
            self.stats.candidate_rtt_ms.append(_ms_since(t0))
            self.stats.candidate_bytes.append(float(len(r.content)))
            self.stats.candidate_framing[body_framing(r.headers)] += 1
        return r.content

    def photo(self, sid: str) -> dict[str, Any]:
        return self._json(self.http.get(f"/v1/sessions/{sid}/photo"))

    def cancel(self, sid: str) -> None:
        with contextlib.suppress(httpx.HTTPError):
            self.http.post(f"/v1/sessions/{sid}/cancel")


def wait_ready(edge: EdgeClient, max_sec: float, stats: Stats | None) -> dict[str, Any]:
    """起動時の hello。max_sec > 0 なら 200 が返るまで最大 max_sec 秒繰り返す (cold start の計測)。

    0 なら従来どおり 1 回だけ試す。最初の要求から 200 までの時間と各応答を stats に残す。
    max_sec は全体の上限: 再試行の待ちと各 hello のタイムアウトを残り時間以下に切り詰める。
    """
    t0 = time.monotonic()
    deadline = t0 + max_sec
    codes: list[str] = []

    def record(ok: bool) -> None:
        if stats:
            key = "ms_to_200" if ok else "ms"
            stats.cold_hello = {
                "ok": ok,
                key: round(_ms_since(t0)),
                "attempts": len(codes),
                "codes": list(codes),
            }

    while True:
        timeout: float | None = None
        if max_sec > 0:
            timeout = min(HELLO_TIMEOUT_SEC, deadline - time.monotonic())
        try:
            info = edge.hello(timeout=timeout)
        except (httpx.HTTPError, RuntimeError) as exc:
            codes.append(error_key(exc))
            if max_sec <= 0 or deadline - time.monotonic() <= 0:
                record(False)
                raise
            print(f"hello: {error_key(exc)} (retrying)", file=sys.stderr)
            time.sleep(min(HELLO_RETRY_SEC, deadline - time.monotonic()))
            if deadline - time.monotonic() <= 0:
                record(False)
                raise
            continue
        codes.append("200")
        record(True)
        if stats:
            # 起動時の hello (cold かもしれない) は warm の RTT に混ぜない
            stats.hello_rtt_ms.clear()
        return info


# ---- 画面 ----


def grab(cap: cv2.VideoCapture, opt: Options) -> np.ndarray:
    """webcam から 1 枚取り、device と同じ解像度の RGB にする。"""
    ok, bgr = cap.read()
    if not ok or bgr is None:
        raise RuntimeError("camera read failed")
    small = cv2.resize(bgr, (opt.width, opt.height), interpolation=cv2.INTER_AREA)
    return cv2.cvtColor(small, cv2.COLOR_BGR2RGB)


_HEADLESS = False  # --headless のときはウィンドウを出さない (main で設定)


def show(rgb: np.ndarray, lines: list[tuple[str, tuple[int, int, int]]]) -> int:
    if _HEADLESS:
        return -1
    view = cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR)
    view = cv2.resize(view, (rgb.shape[1] * 2, rgb.shape[0] * 2), interpolation=cv2.INTER_NEAREST)
    y = 28
    for text, color in lines:
        cv2.putText(view, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.7, (0, 0, 0), 4, cv2.LINE_AA)
        cv2.putText(view, text, (10, y), cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv2.LINE_AA)
        y += 28
    cv2.imshow(WINDOW, view)
    key = cv2.waitKey(1) & 0xFF
    if key in (ord("q"), KEY_ESC):
        raise Quit()
    return key


def result_lines(res: dict[str, Any] | None, phase: str, left: float) -> list[tuple[str, Any]]:
    head = (f"{phase.upper()}  left {left:4.1f}s", YELLOW)
    if res is None:
        return [head, ("edge: no response", RED)]
    if res.get("dropped"):
        return [head, ("dropped", RED)]
    ok = res["all_in_frame"] and res["all_eyes_open"] and res["all_smiling"]
    return [
        head,
        (f"faces {res['face_count']} / target {res['target_face_count']}", WHITE),
        (
            f"in_frame {int(res['all_in_frame'])}  eyes {int(res['all_eyes_open'])}  "
            f"smile {int(res['all_smiling'])}",
            GREEN if ok else WHITE,
        ),
        (f"servo_dx,dy {res['servo_dx']:+d},{res['servo_dy']:+d}  hint {res['hint']}", WHITE),
        (f"latency {res['latency_ms']} ms", WHITE),
    ]


def decode_jpeg_rgb(data: bytes, opt: Options) -> np.ndarray:
    bgr = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if bgr is None:
        raise RuntimeError("candidate jpeg decode failed")
    if bgr.shape[1] != opt.width or bgr.shape[0] != opt.height:
        bgr = cv2.resize(bgr, (opt.width, opt.height), interpolation=cv2.INTER_AREA)
    return cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)


# ---- フロー ----


def accept_result(
    res: dict[str, Any] | None, phase: str, elapsed: float, countdown_sec: float
) -> bool:
    """frame_result の accepted を採用してよいか。

    device と同じく、CAPTURE 開始から countdown_sec 経過後に返った accepted は無視する
    (spec §6.2「10 秒の終了後に届いた結果は採用しない」、protocol.md frame_result)。
    """
    return (
        res is not None
        and bool(res.get("accepted"))
        and phase == "capture"
        and elapsed < countdown_sec
    )


def run_phases(
    edge: EdgeClient,
    cap: cv2.VideoCapture,
    sid: str,
    opt: Options,
    kept: dict[int, np.ndarray],
    next_id: list[int],
    tick: Callable[[], None] | None = None,
) -> bool:
    """COMPOSE → CAPTURE を回す。accepted なら True。"""
    t_start = time.monotonic()
    fs = edge.stats.frame(opt.fmt) if edge.stats else None
    for phase, duration in (("compose", opt.compose_sec), ("capture", opt.capture_sec)):
        t_phase = time.monotonic()
        period = 1.0 / opt.fps
        prev_send: float | None = None
        try:
            while time.monotonic() - t_phase < duration:
                t_frame = time.monotonic()
                if fs and prev_send is not None:
                    fs.interval_ms.append((t_frame - prev_send) * 1000.0)
                prev_send = t_frame
                rgb = grab(cap, opt)
                fid = next_id[0]
                next_id[0] += 1
                capture_ms = int((t_frame - t_start) * 1000)
                res: dict[str, Any] | None
                try:
                    res = edge.frame(sid, fid, capture_ms, rgb, opt, phase)
                except (httpx.HTTPError, RuntimeError) as exc:
                    print(f"frame {fid}: {exc}", file=sys.stderr)
                    if (
                        opt.abort_on_lost
                        and isinstance(exc, EdgeHTTPError)
                        and exc.code == "unknown_session"
                    ):
                        raise SessionLost(str(exc)) from exc
                    res = None  # device と同じく再送しない
                if phase == "capture":
                    kept[fid] = rgb
                    for old in [k for k in kept if k < fid - 100]:
                        del kept[old]
                elapsed = time.monotonic() - t_phase
                show(rgb, result_lines(res, phase, max(0.0, duration - elapsed)))
                if accept_result(res, phase, elapsed, duration):
                    show(rgb, [("captured!", GREEN)])
                    if not _HEADLESS:
                        cv2.waitKey(800)
                    return True
                if phase == "capture" and elapsed >= duration:
                    break  # 10 秒経過後に返った結果 (accepted を含む) は使わず timeout へ
                if tick:
                    tick()
                time.sleep(max(0.0, period - (time.monotonic() - t_frame)))
        finally:
            if fs:
                fs.loop_sec += time.monotonic() - t_phase
    return False


def save_and_poll(edge: EdgeClient, sid: str, opt: Options, rgb: np.ndarray | None) -> int:
    t0 = time.monotonic()
    try:
        edge.review(sid, "save")
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            p = edge.photo(sid)
            if p["status"] == "ready":
                if edge.stats:
                    edge.stats.photo_ready_ms.append(_ms_since(t0))
                print(f"photo_url:  {p['photo_url']}")
                print(f"share_url:  {p['share_url']}")
                print(f"expires_at: {p['expires_at']}")
                if opt.open_browser:
                    webbrowser.open(p["photo_url"])
                return 0
            if p["status"] == "error":
                print(f"upload error: {p.get('reason')}", file=sys.stderr)
                if edge.stats:
                    edge.stats.save_errors[f"upload:{p.get('reason')}"] += 1
                return 1
            if rgb is not None:
                show(rgb, [("uploading...", YELLOW)])
            time.sleep(0.5)
    except (httpx.HTTPError, RuntimeError) as exc:
        if edge.stats:
            edge.stats.save_errors[error_key(exc)] += 1
        raise
    print("photo: timed out waiting for ready", file=sys.stderr)
    if edge.stats:
        edge.stats.save_errors["timeout"] += 1
    return 1


def candidate_image(
    edge: EdgeClient, sid: str, cand: dict[str, Any], kept: dict[int, np.ndarray], opt: Options
) -> np.ndarray:
    """REVIEW に出す候補。

    --candidate-via-edge なら edge の JPEG、そうでなければ手元の保持フレーム。
    """
    if opt.candidate_via_edge:
        try:
            return decode_jpeg_rgb(edge.candidate(sid), opt)
        except (httpx.HTTPError, RuntimeError) as exc:
            print(f"candidate: {exc}", file=sys.stderr)
    return kept.get(cand["frame_id"], np.zeros((opt.height, opt.width, 3), np.uint8))


def session(edge: EdgeClient, cap: cv2.VideoCapture, opt: Options) -> int:
    sid = str(uuid.uuid4())
    edge.start(sid)
    kept: dict[int, np.ndarray] = {}
    next_id = [1]
    try:
        while True:
            if run_phases(edge, cap, sid, opt, kept, next_id):
                return save_and_poll(edge, sid, opt, None)
            cand = edge.timeout(sid)["candidate"]
            if cand:
                print(f"timeout candidate: frame {cand['frame_id']} ({cand['reason']})")
                img = candidate_image(edge, sid, cand, kept, opt)
                lines = [("time up: candidate", YELLOW), (cand["reason"], WHITE)]
                lines.append(("s: save  r: retake  q: quit", WHITE))
            else:
                print("timeout: no face found")
                img = grab(cap, opt)
                lines = [("time up: no face found", RED), ("r: retake  q: quit", WHITE)]
            while True:
                key = show(img, lines)
                if key == ord("s") and cand:
                    return save_and_poll(edge, sid, opt, img)
                if key == ord("r"):
                    edge.review(sid, "retake")
                    kept.clear()
                    break
                time.sleep(0.03)
    except Quit:
        edge.cancel(sid)
        raise


def auto_session(
    edge: EdgeClient, cap: cv2.VideoCapture, opt: Options, auto_save: bool, heartbeat_sec: float
) -> str:
    """無人モードの 1 回分。撮影 → (候補取得) → 保存か cancel。結果の種類を返す。"""
    sid = str(uuid.uuid4())
    edge.start(sid)
    kept: dict[int, np.ndarray] = {}
    try:
        accepted = run_phases(
            edge, cap, sid, opt, kept, [1], tick=lambda: edge.heartbeat(heartbeat_sec)
        )
        cand: dict[str, Any] | None = None
        if not accepted:
            cand = edge.timeout(sid)["candidate"]
            if cand is None:
                return "timeout_no_face"
        if opt.candidate_via_edge:
            with contextlib.suppress(httpx.HTTPError, RuntimeError):
                edge.candidate(sid)
        if auto_save:
            # アップロード失敗・ready 待ちの時間切れは 1 が返る。保存できた回と分けて数える
            return "saved" if save_and_poll(edge, sid, opt, None) == 0 else "save_failed"
        return "accepted" if accepted else "timeout_candidate"
    finally:
        edge.cancel(sid)  # 保存しなかった候補を edge に残さない (保存後の cancel は無害)


def auto_loop(
    edge: EdgeClient,
    cap: cv2.VideoCapture,
    opt: Options,
    duration_sec: float,
    auto_save: bool,
    heartbeat_sec: float,
) -> int:
    """--auto-capture: duration_sec の間、撮影を繰り返す。セッションを失ったら撮り直す。"""
    stats = edge.stats
    deadline = time.monotonic() + duration_sec
    while time.monotonic() < deadline:
        try:
            outcome = auto_session(edge, cap, opt, auto_save, heartbeat_sec)
            print(f"session: {outcome}")
        except SessionLost as exc:
            # device なら ERROR「接続が切れました」→ 撮り直し
            outcome = "lost"
            print(f"session lost: {exc}", file=sys.stderr)
            if stats:
                stats.event("session_lost", reason="unknown_session")
        except (httpx.HTTPError, RuntimeError) as exc:
            outcome = f"error:{error_key(exc)}"
            print(f"session error: {exc}", file=sys.stderr)
            if stats:
                stats.event("session_error", reason=error_key(exc))
            time.sleep(1.0)
        if stats:
            stats.sessions[outcome] += 1
        edge.heartbeat(heartbeat_sec)
    return 0


def write_stats(path: str, stats: Stats, meta: dict[str, Any]) -> None:
    with open(path, "w", encoding="utf-8") as fp:
        json.dump(stats.to_json(meta), fp, ensure_ascii=False, indent=2)
        fp.write("\n")
    print(f"stats: {path}")


def main() -> int:
    global _HEADLESS
    ap = argparse.ArgumentParser(description="webcam で device の代わりをする疑似デバイス")
    ap.add_argument("--edge", default="http://127.0.0.1:8765")
    ap.add_argument("--device-id", default="stackchan-01")
    ap.add_argument("--key", default=os.environ.get("EDGE_DEVICE_KEY", "change-me"))
    ap.add_argument("--camera", type=int, default=0)
    ap.add_argument("--format", choices=("jpeg", "rgb565"), default="jpeg")
    ap.add_argument(
        "--byte-order",
        choices=("little", "big"),
        default="little",
        help="--format rgb565 のバイト順。edge の [capture] rgb565_byte_order と合わせる",
    )
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--compose-sec", type=float, default=5.0)
    ap.add_argument("--open", action="store_true", help="保存後に写真ページをブラウザで開く")
    ap.add_argument(
        "--stats", metavar="PATH", help="終了時に計測結果 (件数・p50・p95・max) を JSON で書く"
    )
    ap.add_argument(
        "--candidate-via-edge",
        action="store_true",
        help="候補の表示に edge の GET …/candidate を使う (device と同じ経路)",
    )
    ap.add_argument(
        "--wait-ready-sec",
        type=float,
        default=0.0,
        help="起動時の hello を 200 が返るまで最大この秒数繰り返す (0 なら 1 回だけ)",
    )
    ap.add_argument(
        "--headless", action="store_true", help="ウィンドウを出さない (--auto-capture と併用)"
    )
    ap.add_argument("--auto-capture", action="store_true", help="撮影開始を自動で繰り返す")
    ap.add_argument("--duration-sec", type=float, default=600.0, help="--auto-capture を回す時間")
    ap.add_argument(
        "--heartbeat-sec", type=float, default=5.0, help="--auto-capture 中の hello の間隔"
    )
    ap.add_argument(
        "--auto-save",
        action="store_true",
        help="--auto-capture で候補があれば gallery に保存する (既定は保存せず cancel)",
    )
    args = ap.parse_args()
    if args.headless and not args.auto_capture:
        print("--headless は --auto-capture と一緒に使ってください", file=sys.stderr)
        return 2
    if cv2 is None:
        print("OpenCV (cv2) を import できません。uv sync を確認してください", file=sys.stderr)
        return 1
    _HEADLESS = args.headless

    stats = Stats() if args.stats else None
    meta = {
        "edge_host": httpx.URL(args.edge).host,
        "format": args.format,
        "fps_target": args.fps,
        "size": f"{args.width}x{args.height}",
        "mode": "auto" if args.auto_capture else "interactive",
    }
    edge = EdgeClient(args.edge, args.device_id, args.key, stats=stats)
    try:
        try:
            info = wait_ready(edge, args.wait_ready_sec, stats)
        except (httpx.HTTPError, RuntimeError) as exc:
            print(f"hello failed: {exc}", file=sys.stderr)
            return 1
        print(f"hello: {info}")
        opt = Options(
            fmt=args.format,
            byte_order=args.byte_order,
            width=args.width,
            height=args.height,
            fps=args.fps,
            compose_sec=args.compose_sec,
            capture_sec=float(info.get("countdown_sec", 10)),
            open_browser=args.open,
            candidate_via_edge=args.candidate_via_edge,
            headless=args.headless,
            abort_on_lost=args.auto_capture,
        )

        cap = cv2.VideoCapture(args.camera)
        if not cap.isOpened():
            print(f"camera {args.camera} could not be opened", file=sys.stderr)
            return 1
        try:
            if args.auto_capture:
                return auto_loop(
                    edge, cap, opt, args.duration_sec, args.auto_save, args.heartbeat_sec
                )
            while True:
                key = show(grab(cap, opt), [("press SPACE to start", YELLOW), ("q: quit", WHITE)])
                if key == KEY_SPACE:
                    return session(edge, cap, opt)
        except (Quit, KeyboardInterrupt):
            return 0
        finally:
            cap.release()
            if not args.headless:
                cv2.destroyAllWindows()
    finally:
        if stats is not None:
            meta["ended_at"] = datetime.now(UTC).isoformat()
            write_stats(args.stats, stats, meta)


if __name__ == "__main__":
    sys.exit(main())
