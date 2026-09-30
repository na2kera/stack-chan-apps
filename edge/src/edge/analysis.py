"""顔解析: FaceObservation、FaceAnalyzer Protocol、MediaPipeAnalyzer。

mediapipe は MediaPipeAnalyzer の生成時にだけ import する。
テストや decision.py / head.py は FaceObservation だけを使い、mediapipe を読み込まない。
"""

from __future__ import annotations

import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

import numpy as np

BLINK_LEFT = "eyeBlinkLeft"
BLINK_RIGHT = "eyeBlinkRight"
SMILE_LEFT = "mouthSmileLeft"
SMILE_RIGHT = "mouthSmileRight"


@dataclass(frozen=True)
class FaceObservation:
    bbox: tuple[float, float, float, float]  # 正規化 (x0, y0, x1, y1)。ランドマークの min/max
    eye_blink_left: float  # blendshape スコア 0..1
    eye_blink_right: float
    mouth_smile_left: float
    mouth_smile_right: float


class FaceAnalyzer(Protocol):
    def analyze(self, rgb: np.ndarray, timestamp_ms: int) -> list[FaceObservation]: ...


class ModelNotFoundError(FileNotFoundError):
    pass


def observation_from_landmarks(landmarks: Any, blendshapes: Any) -> FaceObservation:
    """MediaPipe の 1 顔分の結果を FaceObservation にする。

    bbox はクリップしない (画像外へのはみ出しを「見切れ」として判定に使うため)。
    """
    xs = [lm.x for lm in landmarks]
    ys = [lm.y for lm in landmarks]
    scores = {c.category_name: float(c.score) for c in (blendshapes or [])}
    return FaceObservation(
        bbox=(float(min(xs)), float(min(ys)), float(max(xs)), float(max(ys))),
        eye_blink_left=scores.get(BLINK_LEFT, 1.0),  # 取れなければ「閉じている」扱いで安全側
        eye_blink_right=scores.get(BLINK_RIGHT, 1.0),
        mouth_smile_left=scores.get(SMILE_LEFT, 0.0),
        mouth_smile_right=scores.get(SMILE_RIGHT, 0.0),
    )


class MediaPipeAnalyzer:
    """MediaPipe Face Landmarker (Tasks API, VIDEO モード)。起動時に 1 回だけモデルをロードする。

    max_faces は判定と UI の上限。検出は max_faces + 1 人まで行い、超過を decision.py が拒否する。

    VIDEO モードはタイムスタンプが単調増加でないと例外になる。セッションをまたいでも
    増え続けるよう、渡された値が前回以下なら前回 + 1 に繰り上げる。
    MediaPipe のグラフはスレッドセーフではないので、呼び出しはロックで直列化する。
    """

    def __init__(self, model_path: Path, max_faces: int, warmup: bool = True) -> None:
        if not model_path.is_file():
            raise ModelNotFoundError(
                f"model not found: {model_path}. run `uv run python tools/download_model.py`"
            )
        import mediapipe as mp  # 重いので必要になったときだけ読む

        self._mp = mp
        vision = mp.tasks.vision
        options = vision.FaceLandmarkerOptions(
            base_options=mp.tasks.BaseOptions(
                model_asset_path=str(model_path),
                delegate=mp.tasks.BaseOptions.Delegate.CPU,  # PC GPU を必須にしない (spec §3)
            ),
            running_mode=vision.RunningMode.VIDEO,
            # 上限 + 1 人まで検出し、上限超え (too_many) を判定できるようにする
            num_faces=max_faces + 1,
            output_face_blendshapes=True,
        )
        self._landmarker = vision.FaceLandmarker.create_from_options(options)
        self._lock = threading.Lock()
        self._last_ts = -1
        if warmup:
            # グラフの初期化失敗を最初のフレームではなく起動時に表面化させる
            self.analyze(np.zeros((240, 320, 3), dtype=np.uint8), 0)

    def analyze(self, rgb: np.ndarray, timestamp_ms: int) -> list[FaceObservation]:
        mp = self._mp
        image = mp.Image(image_format=mp.ImageFormat.SRGB, data=np.ascontiguousarray(rgb))
        with self._lock:
            ts = max(int(timestamp_ms), self._last_ts + 1)
            self._last_ts = ts
            result = self._landmarker.detect_for_video(image, ts)
        blend = result.face_blendshapes or []
        return [
            observation_from_landmarks(lms, blend[i] if i < len(blend) else None)
            for i, lms in enumerate(result.face_landmarks or [])
        ]

    def close(self) -> None:
        with self._lock:
            self._landmarker.close()
