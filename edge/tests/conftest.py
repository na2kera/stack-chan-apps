"""テスト共通: 偽の解析器、設定、TestClient。モデルもハードウェアも使わない。"""

from __future__ import annotations

import uuid
from collections.abc import Callable, Iterable
from concurrent.futures import Future
from datetime import UTC, datetime, timedelta
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from fastapi.testclient import TestClient

from edge.analysis import FaceObservation
from edge.api import create_app
from edge.config import Config, load_config
from edge.gallery import MockGallery
from edge.image import encode_jpeg, encode_rgb565
from edge.session import PhotoboothService, SessionStore

EDGE_DIR = Path(__file__).resolve().parent.parent
DEVICE_ID = "stackchan-01"
DEVICE_KEY = "test-key"


def face(
    x0: float = 0.4,
    y0: float = 0.35,
    x1: float = 0.6,
    y1: float = 0.65,
    blink: float = 0.05,
    smile: float = 0.8,
    *,
    blink_left: float | None = None,
    blink_right: float | None = None,
    smile_left: float | None = None,
    smile_right: float | None = None,
) -> FaceObservation:
    """既定値は「枠内・開眼・笑顔」の 1 顔。"""
    return FaceObservation(
        bbox=(x0, y0, x1, y1),
        eye_blink_left=blink if blink_left is None else blink_left,
        eye_blink_right=blink if blink_right is None else blink_right,
        mouth_smile_left=smile if smile_left is None else smile_left,
        mouth_smile_right=smile if smile_right is None else smile_right,
    )


def two_faces(**kw: Any) -> list[FaceObservation]:
    return [face(0.2, 0.35, 0.4, 0.65, **kw), face(0.6, 0.35, 0.8, 0.65, **kw)]


class FakeAnalyzer:
    """呼ばれるたびに観測列から 1 つ取り出して返す。尽きたら最後の値を返し続ける。"""

    def __init__(self, script: Iterable[list[FaceObservation]] = ()) -> None:
        self.script = list(script)
        self.calls = 0
        self.timestamps: list[int] = []
        self.last: list[FaceObservation] = []

    def push(self, *frames: list[FaceObservation]) -> None:
        self.script.extend(frames)

    def analyze(self, rgb: np.ndarray, timestamp_ms: int) -> list[FaceObservation]:
        self.timestamps.append(timestamp_ms)
        self.calls += 1
        if self.script:
            self.last = self.script.pop(0)
        return self.last


class SyncExecutor:
    """アップロードをその場で実行する (テストを決定的にする)。"""

    def submit(self, fn: Callable[..., Any], *args: Any, **kwargs: Any) -> Future:
        fut: Future = Future()
        try:
            fut.set_result(fn(*args, **kwargs))
        except Exception as exc:  # pragma: no cover
            fut.set_exception(exc)
        return fut


class FakeClock:
    def __init__(self, start: datetime | None = None) -> None:
        self.now = start or datetime(2026, 9, 30, 12, 0, tzinfo=UTC)

    def __call__(self) -> datetime:
        return self.now

    def advance(self, **kw: float) -> None:
        self.now += timedelta(**kw)


class FakeMonotonic:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


@pytest.fixture
def cfg() -> Config:
    return load_config(EDGE_DIR / "config.example.toml", env={"EDGE_DEVICE_KEY": DEVICE_KEY})


@pytest.fixture
def analyzer() -> FakeAnalyzer:
    return FakeAnalyzer()


@pytest.fixture
def clock() -> FakeClock:
    return FakeClock()


@pytest.fixture
def mono() -> FakeMonotonic:
    return FakeMonotonic()


@pytest.fixture
def gallery(cfg: Config, clock: FakeClock) -> MockGallery:
    return MockGallery(
        base_url="http://edge.test:8765",
        ttl=timedelta(minutes=cfg.gallery.ttl_minutes),
        share_text=cfg.share.text,
        now=clock,
    )


@pytest.fixture
def service(
    cfg: Config, analyzer: FakeAnalyzer, gallery: MockGallery, clock: FakeClock, mono: FakeMonotonic
) -> PhotoboothService:
    return PhotoboothService(
        cfg,
        analyzer,
        gallery,
        store=SessionStore(clock=mono, max_sessions=cfg.server.max_sessions),
        executor=SyncExecutor(),  # type: ignore[arg-type]
        wall_clock=clock,
        clock_ms=lambda: int(mono.t * 1000),
    )


@pytest.fixture
def client(service: PhotoboothService, cfg: Config, gallery: MockGallery) -> TestClient:
    app = create_app(service, cfg.auth, gallery, background_sweep=False)
    return TestClient(app)


AUTH = {"X-Device-Id": DEVICE_ID, "X-Device-Key": DEVICE_KEY}


def sample_image(width: int = 32, height: int = 24) -> np.ndarray:
    rng = np.random.default_rng(0)
    return rng.integers(0, 255, (height, width, 3), dtype=np.uint8)


def frame_headers(
    frame_id: int,
    phase: str,
    fmt: str = "jpeg",
    width: int = 32,
    height: int = 24,
    capture_ms: int | None = None,
    servo_x: int = 0,
    servo_y: int = 450,
) -> dict[str, str]:
    return {
        **AUTH,
        "Content-Type": "application/octet-stream",
        "X-Frame-Id": str(frame_id),
        "X-Capture-Ms": str(capture_ms if capture_ms is not None else frame_id * 200),
        "X-Servo-X": str(servo_x),
        "X-Servo-Y": str(servo_y),
        "X-Width": str(width),
        "X-Height": str(height),
        "X-Format": fmt,
        "X-Phase": phase,
    }


def jpeg_body() -> bytes:
    return encode_jpeg(sample_image(), 80)


def rgb565_body(byte_order: str = "little") -> bytes:
    return encode_rgb565(sample_image(), byte_order)  # type: ignore[arg-type]


def new_session_id() -> str:
    return str(uuid.uuid4())
