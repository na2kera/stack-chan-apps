"""疑似デバイス: PC の webcam で K151 と同じ HTTP フロー (docs/protocol.md) を回す (design §8)。

使い方:
    uv run python tools/webcam_device.py --edge http://127.0.0.1:8765 \
        --device-id stackchan-01 --key <EDGE_DEVICE_KEY> [--format rgb565] [--open]

操作: SPACE で撮影開始 / 候補表示中は s で保存・r で撮り直し / q か ESC で終了。
servo_dx / servo_dy は webcam では動かせないので表示だけ。
edge 本体では OpenCV を使わない。cv2 を import するのはこのツールだけ。
"""

from __future__ import annotations

import argparse
import contextlib
import os
import sys
import time
import uuid
import webbrowser
from dataclasses import dataclass
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


class Quit(Exception):
    pass


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


class EdgeClient:
    def __init__(self, base_url: str, device_id: str, key: str) -> None:
        self.device_id = device_id
        self.http = httpx.Client(
            base_url=base_url.rstrip("/"),
            headers={"X-Device-Id": device_id, "X-Device-Key": key},
            timeout=3.0,  # device と同じ 3 秒
        )

    def _json(self, r: httpx.Response) -> dict[str, Any]:
        if r.status_code >= 400:
            raise RuntimeError(
                f"{r.request.method} {r.request.url.path} -> {r.status_code} {r.text}"
            )
        return r.json()

    def hello(self) -> dict[str, Any]:
        body = {"device_id": self.device_id, "protocol_version": 1}
        return self._json(self.http.post("/v1/hello", json=body))

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
        return self._json(
            self.http.post(f"/v1/sessions/{sid}/frames", content=body, headers=headers)
        )

    def timeout(self, sid: str) -> dict[str, Any]:
        return self._json(self.http.post(f"/v1/sessions/{sid}/timeout"))

    def review(self, sid: str, decision: str) -> dict[str, Any]:
        return self._json(self.http.post(f"/v1/sessions/{sid}/review", json={"decision": decision}))

    def photo(self, sid: str) -> dict[str, Any]:
        return self._json(self.http.get(f"/v1/sessions/{sid}/photo"))

    def cancel(self, sid: str) -> None:
        with contextlib.suppress(httpx.HTTPError):
            self.http.post(f"/v1/sessions/{sid}/cancel")


# ---- 画面 ----


def grab(cap: cv2.VideoCapture, opt: Options) -> np.ndarray:
    """webcam から 1 枚取り、device と同じ解像度の RGB にする。"""
    ok, bgr = cap.read()
    if not ok or bgr is None:
        raise RuntimeError("camera read failed")
    small = cv2.resize(bgr, (opt.width, opt.height), interpolation=cv2.INTER_AREA)
    return cv2.cvtColor(small, cv2.COLOR_BGR2RGB)


def show(rgb: np.ndarray, lines: list[tuple[str, tuple[int, int, int]]]) -> int:
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
) -> bool:
    """COMPOSE → CAPTURE を回す。accepted なら True。"""
    t_start = time.monotonic()
    for phase, duration in (("compose", opt.compose_sec), ("capture", opt.capture_sec)):
        t_phase = time.monotonic()
        period = 1.0 / opt.fps
        while time.monotonic() - t_phase < duration:
            t_frame = time.monotonic()
            rgb = grab(cap, opt)
            fid = next_id[0]
            next_id[0] += 1
            capture_ms = int((t_frame - t_start) * 1000)
            res: dict[str, Any] | None
            try:
                res = edge.frame(sid, fid, capture_ms, rgb, opt, phase)
            except (httpx.HTTPError, RuntimeError) as exc:
                print(f"frame {fid}: {exc}", file=sys.stderr)
                res = None  # device と同じく再送しない
            if phase == "capture":
                kept[fid] = rgb
                for old in [k for k in kept if k < fid - 100]:
                    del kept[old]
            elapsed = time.monotonic() - t_phase
            show(rgb, result_lines(res, phase, max(0.0, duration - elapsed)))
            if accept_result(res, phase, elapsed, duration):
                show(rgb, [("captured!", GREEN)])
                cv2.waitKey(800)
                return True
            if phase == "capture" and elapsed >= duration:
                break  # 10 秒経過後に返った結果 (accepted を含む) は使わず timeout へ
            time.sleep(max(0.0, period - (time.monotonic() - t_frame)))
    return False


def save_and_poll(edge: EdgeClient, sid: str, opt: Options, rgb: np.ndarray | None) -> int:
    edge.review(sid, "save")
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        p = edge.photo(sid)
        if p["status"] == "ready":
            print(f"photo_url:  {p['photo_url']}")
            print(f"share_url:  {p['share_url']}")
            print(f"expires_at: {p['expires_at']}")
            if opt.open_browser:
                webbrowser.open(p["photo_url"])
            return 0
        if p["status"] == "error":
            print(f"upload error: {p.get('reason')}", file=sys.stderr)
            return 1
        if rgb is not None:
            show(rgb, [("uploading...", YELLOW)])
        time.sleep(0.5)
    print("photo: timed out waiting for ready", file=sys.stderr)
    return 1


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
                img = kept.get(cand["frame_id"], np.zeros((opt.height, opt.width, 3), np.uint8))
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


def main() -> int:
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
    args = ap.parse_args()
    if cv2 is None:
        print("OpenCV (cv2) を import できません。uv sync を確認してください", file=sys.stderr)
        return 1

    edge = EdgeClient(args.edge, args.device_id, args.key)
    try:
        info = edge.hello()
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
    )

    cap = cv2.VideoCapture(args.camera)
    if not cap.isOpened():
        print(f"camera {args.camera} could not be opened", file=sys.stderr)
        return 1
    try:
        while True:
            key = show(grab(cap, opt), [("press SPACE to start", YELLOW), ("q: quit", WHITE)])
            if key == KEY_SPACE:
                return session(edge, cap, opt)
    except Quit:
        return 0
    finally:
        cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    sys.exit(main())
