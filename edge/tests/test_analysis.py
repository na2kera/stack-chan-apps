"""MediaPipeAnalyzer の周辺ロジック (mediapipe もモデルも使わない)。"""

from __future__ import annotations

import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest
from conftest import AUTH, face, frame_headers, jpeg_body, new_session_id
from fastapi.testclient import TestClient

from edge.analysis import MediaPipeAnalyzer, ModelNotFoundError, observation_from_landmarks


def _lm(x: float, y: float) -> SimpleNamespace:
    return SimpleNamespace(x=x, y=y, z=0.0)


def _cat(name: str, score: float) -> SimpleNamespace:
    return SimpleNamespace(category_name=name, score=score)


def test_observation_from_landmarks_unclipped_bbox() -> None:
    lms = [_lm(0.3, 0.2), _lm(0.5, 0.6), _lm(-0.05, 0.4)]
    blend = [
        _cat("eyeBlinkLeft", 0.1),
        _cat("eyeBlinkRight", 0.2),
        _cat("mouthSmileLeft", 0.7),
        _cat("mouthSmileRight", 0.8),
        _cat("jawOpen", 0.9),
    ]
    obs = observation_from_landmarks(lms, blend)
    assert obs.bbox == (-0.05, 0.2, 0.5, 0.6)  # 画像外へのはみ出しを残す
    assert (obs.eye_blink_left, obs.eye_blink_right) == (0.1, 0.2)
    assert (obs.mouth_smile_left, obs.mouth_smile_right) == (0.7, 0.8)


def test_missing_blendshapes_are_safe_side() -> None:
    obs = observation_from_landmarks([_lm(0.4, 0.4), _lm(0.6, 0.6)], None)
    assert obs.eye_blink_left == 1.0 and obs.mouth_smile_left == 0.0


def test_model_missing(tmp_path: Path) -> None:
    with pytest.raises(ModelNotFoundError):
        MediaPipeAnalyzer(tmp_path / "none.task", 4)


def test_video_timestamps_forced_monotonic(monkeypatch: pytest.MonkeyPatch) -> None:
    """VIDEO モードに渡すタイムスタンプはセッションをまたいでも厳密に増える。"""
    seen: list[int] = []

    class FakeLandmarker:
        def detect_for_video(self, image, ts):
            assert not seen or ts > seen[-1]
            seen.append(ts)
            return SimpleNamespace(face_landmarks=[], face_blendshapes=[])

    fake_mp = SimpleNamespace(
        Image=lambda image_format, data: data,
        ImageFormat=SimpleNamespace(SRGB="srgb"),
    )
    a = MediaPipeAnalyzer.__new__(MediaPipeAnalyzer)
    a._mp = fake_mp
    a._landmarker = FakeLandmarker()
    a._lock = threading.Lock()
    a._last_ts = -1
    img = np.zeros((4, 4, 3), dtype=np.uint8)
    for ts in (100, 100, 50, 0, 200):
        assert a.analyze(img, ts) == []
    assert seen == [100, 101, 102, 103, 200]


def test_concurrent_frames_same_session(client: TestClient, analyzer) -> None:
    """同じセッションに並行でフレームが来ても 1 枚ずつ処理され、古い frame_id は落ちる。"""
    analyzer.push(*([[face()]] * 50))
    sid = new_session_id()
    client.post("/v1/sessions", json={"session_id": sid}, headers=AUTH)

    def post(i: int) -> dict:
        r = client.post(
            f"/v1/sessions/{sid}/frames",
            content=jpeg_body(),
            headers=frame_headers(i, "compose"),
        )
        assert r.status_code == 200
        return r.json()

    with ThreadPoolExecutor(8) as ex:
        results = list(ex.map(post, range(1, 41)))
    processed = [r["frame_id"] for r in results if not r["dropped"]]
    assert processed  # 少なくとも一部は処理される
    assert analyzer.calls == len(processed)  # dropped は解析しない


def test_app_modules_do_not_import_mediapipe_or_cv2() -> None:
    """テスト・CI でモデルや GUI ライブラリを読み込まないこと。"""
    code = (
        "import sys, edge.api, edge.session, edge.decision, edge.head, edge.analysis;"
        "bad = {'mediapipe', 'cv2'} & set(sys.modules); assert not bad, bad"
    )
    subprocess.run([sys.executable, "-c", code], check=True)
