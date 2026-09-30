"""採用判定の純関数 (spec §6.2, §6.4 / design §5)。

FastAPI・mediapipe・numpy に依存しない。検出結果 (FaceObservation の列) と設定を受け取り、
結果と次の状態を返す。状態は frozen dataclass で、呼び出し側 (session.py) が持ち回る。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from collections.abc import Sequence

    from edge.analysis import FaceObservation
    from edge.config import CaptureConfig

HINT_TOO_MANY = "too_many"
HINT_CLOSER = "closer"


@dataclass(frozen=True)
class FrameVerdict:  # frame_result にそのまま写す
    face_count: int
    target_face_count: int
    all_in_frame: bool
    all_eyes_open: bool
    all_smiling: bool
    hint: str | None
    conditions_met: bool


@dataclass(frozen=True)
class CountState:
    """人数の安定と目標人数の追跡状態。"""

    last_count: int | None = None
    run_length: int = 0  # last_count が連続したフレーム数 (今のフレームを含む)
    target: int = 0  # 目標人数。安定した人数の最大値。下げない

    def is_stable(self, cfg: CaptureConfig) -> bool:
        return self.run_length >= cfg.stable_frames


# ---- 顔ごとの判定 ----


def face_in_frame(face: FaceObservation, cfg: CaptureConfig) -> bool:
    """bbox 全体が上下左右 margin の内側にあり、幅が min_face_width_ratio 以上。"""
    x0, y0, x1, y1 = face.bbox
    m = cfg.margin_ratio
    inside = x0 >= m and y0 >= m and x1 <= 1.0 - m and y1 <= 1.0 - m
    return inside and (x1 - x0) >= cfg.min_face_width_ratio


def eyes_open(face: FaceObservation, cfg: CaptureConfig) -> bool:
    return face.eye_blink_left <= cfg.eye_blink_max and face.eye_blink_right <= cfg.eye_blink_max


def smiling(face: FaceObservation, cfg: CaptureConfig) -> bool:
    return (
        face.mouth_smile_left >= cfg.mouth_smile_min
        and face.mouth_smile_right >= cfg.mouth_smile_min
    )


def min_smile(face: FaceObservation) -> float:
    return min(face.mouth_smile_left, face.mouth_smile_right)


def union_bbox(faces: Sequence[FaceObservation]) -> tuple[float, float, float, float] | None:
    if not faces:
        return None
    return (
        min(f.bbox[0] for f in faces),
        min(f.bbox[1] for f in faces),
        max(f.bbox[2] for f in faces),
        max(f.bbox[3] for f in faces),
    )


def group_too_large(faces: Sequence[FaceObservation], cfg: CaptureConfig) -> bool:
    """全顔の外接矩形が margin を除いた幅・高さより大きい (首を振っても全員は入らない)。"""
    u = union_bbox(faces)
    if u is None:
        return False
    usable = 1.0 - 2.0 * cfg.margin_ratio
    return (u[2] - u[0]) > usable or (u[3] - u[1]) > usable


# ---- 人数・目標人数 ----


def update_count(state: CountState, face_count: int, cfg: CaptureConfig) -> CountState:
    """人数の連続回数を数え、stable_frames 以上続いた人数で目標人数を引き上げる。

    COMPOSE / CAPTURE 共通: COMPOSE の間の安定人数の最大値がそのまま CAPTURE の初期目標になり、
    CAPTURE 中に安定して増えれば目標も増える。減っても下げない (spec §6.2)。
    """
    run = state.run_length + 1 if face_count == state.last_count else 1
    target = state.target
    if run >= cfg.stable_frames and face_count > target:
        target = face_count
    return CountState(last_count=face_count, run_length=run, target=target)


# ---- 1 フレームの判定 ----


def evaluate_frame(
    faces: Sequence[FaceObservation],
    prev: CountState,
    cfg: CaptureConfig,
    head_closer: bool = False,
) -> tuple[FrameVerdict, CountState]:
    """1 フレームの判定。head_closer は head.py が「可動域端でまだ寄せたい」と判断したとき True。"""
    n = len(faces)
    state = update_count(prev, n, cfg)
    all_in = n > 0 and all(face_in_frame(f, cfg) for f in faces)
    all_open = n > 0 and all(eyes_open(f, cfg) for f in faces)
    all_smile = n > 0 and all(smiling(f, cfg) for f in faces)

    hint: str | None = None
    if n > cfg.max_faces:
        hint = HINT_TOO_MANY
    elif n > 0 and (group_too_large(faces, cfg) or head_closer):
        hint = HINT_CLOSER

    met = (
        state.is_stable(cfg)  # 人数が前フレームと違う間は保留
        and n >= state.target
        and 1 <= n <= cfg.max_faces
        and all_in
        and all_open
        and all_smile
    )
    verdict = FrameVerdict(
        face_count=n,
        target_face_count=state.target,
        all_in_frame=all_in,
        all_eyes_open=all_open,
        all_smiling=all_smile,
        hint=hint,
        conditions_met=met,
    )
    return verdict, state


def accept_step(consecutive_met: int, conditions_met: bool, cfg: CaptureConfig) -> tuple[int, bool]:
    """条件達成の連続回数を更新し、accept_consecutive に達したら (このフレームを) 採用する。"""
    n = consecutive_met + 1 if conditions_met else 0
    return n, n >= cfg.accept_consecutive


# ---- タイムアウト候補 (spec §6.4) ----


@dataclass(frozen=True, order=True)
class CandidateScore:
    """辞書順で比較する。大きいほど良い。"""

    face_count: int
    in_frame: int
    eyes_open: int
    min_smile: float
    sharpness: float

    def scalar(self) -> float:
        """device に返す 1 つの数値 (大きいほど良い)。人数・枠内・開眼を桁で分け、笑顔を小数に置く。

        ブレ指標は同点決着にしか使わないのでここには含めない。
        """
        return round(
            self.face_count * 1000 + self.in_frame * 100 + self.eyes_open * 10 + self.min_smile, 3
        )

    def reason(self) -> str:
        return (
            f"faces={self.face_count} in_frame={self.in_frame} eyes_open={self.eyes_open} "
            f"min_smile={self.min_smile:.2f} sharpness={self.sharpness:.0f}"
        )


def candidate_score(
    faces: Sequence[FaceObservation], sharpness: float, cfg: CaptureConfig
) -> CandidateScore | None:
    """顔 0 のフレームは候補にしない。"""
    if not faces:
        return None
    return CandidateScore(
        face_count=len(faces),
        in_frame=sum(1 for f in faces if face_in_frame(f, cfg)),
        eyes_open=sum(1 for f in faces if eyes_open(f, cfg)),
        min_smile=min(min_smile(f) for f in faces),
        sharpness=sharpness,
    )


def is_better(new: CandidateScore | None, current: CandidateScore | None) -> bool:
    """new が current より厳密に良ければ True (同点なら先に来た方を残す)。"""
    if new is None:
        return False
    return current is None or new > current
