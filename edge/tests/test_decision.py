from __future__ import annotations

import ast
from pathlib import Path

import pytest
from conftest import face, two_faces

from edge.config import CaptureConfig, Config
from edge.decision import (
    HINT_CLOSER,
    HINT_TOO_MANY,
    CandidateScore,
    CountState,
    accept_step,
    candidate_score,
    evaluate_frame,
    face_in_frame,
    is_better,
    update_count,
)


@pytest.fixture
def cap(cfg: Config) -> CaptureConfig:
    return cfg.capture


def run(frames, cap, state=None):
    """観測列を順に評価し、最後の verdict と状態を返す。"""
    state = state or CountState()
    verdict = None
    for faces in frames:
        verdict, state = evaluate_frame(faces, state, cap)
    return verdict, state


def test_module_is_pure() -> None:
    src = Path(__file__).resolve().parents[1] / "src" / "edge"
    for name in ("decision.py", "head.py"):
        tree = ast.parse((src / name).read_text())
        mods = {
            (n.module or "").split(".")[0] for n in ast.walk(tree) if isinstance(n, ast.ImportFrom)
        } | {
            a.name.split(".")[0]
            for n in ast.walk(tree)
            if isinstance(n, ast.Import)
            for a in n.names
        }
        assert not mods & {"fastapi", "mediapipe", "cv2", "starlette"}, name


def test_all_good_two_frames_met(cap: CaptureConfig) -> None:
    v, _ = run([[face()], [face()]], cap)
    assert v.conditions_met
    assert (v.face_count, v.target_face_count) == (1, 1)
    assert v.all_in_frame and v.all_eyes_open and v.all_smiling and v.hint is None


def test_first_frame_not_met_because_count_unstable(cap: CaptureConfig) -> None:
    v, _ = run([[face()]], cap)
    assert not v.conditions_met


@pytest.mark.parametrize(
    "bbox",
    [
        (0.05, 0.3, 0.3, 0.6),  # 左の余白 8% を割る
        (0.7, 0.3, 0.95, 0.6),  # 右
        (0.4, 0.02, 0.6, 0.4),  # 上
        (0.4, 0.6, 0.6, 0.97),  # 下
        (0.4, 0.3, 0.6, 1.1),  # 画像外へはみ出し
    ],
)
def test_out_of_frame(cap: CaptureConfig, bbox) -> None:
    f = face(*bbox)
    assert not face_in_frame(f, cap)
    v, _ = run([[f], [f]], cap)
    assert not v.all_in_frame and not v.conditions_met


def test_margin_boundary_is_inside(cap: CaptureConfig) -> None:
    assert face_in_frame(face(0.08, 0.08, 0.92, 0.92), cap)


def test_face_too_small(cap: CaptureConfig) -> None:
    small = face(0.45, 0.45, 0.52, 0.55)  # 幅 7% < 8%
    assert not face_in_frame(small, cap)
    v, _ = run([[small], [small]], cap)
    assert not v.conditions_met


@pytest.mark.parametrize("kw", [{"blink_left": 0.6}, {"blink_right": 0.26}])
def test_one_eye_closed(cap: CaptureConfig, kw) -> None:
    v, _ = run([[face(**kw)], [face(**kw)]], cap)
    assert not v.all_eyes_open and not v.conditions_met


def test_blink_threshold_inclusive(cap: CaptureConfig) -> None:
    v, _ = run([[face(blink=0.25, smile=0.55)]] * 2, cap)
    assert v.all_eyes_open and v.all_smiling and v.conditions_met


@pytest.mark.parametrize("kw", [{"smile_left": 0.2}, {"smile_right": 0.34}])
def test_one_side_not_smiling(cap: CaptureConfig, kw) -> None:
    v, _ = run([[face(**kw)], [face(**kw)]], cap)
    assert not v.all_smiling and not v.conditions_met


def test_one_of_two_not_smiling(cap: CaptureConfig) -> None:
    faces = [face(0.2, 0.35, 0.4, 0.65), face(0.6, 0.35, 0.8, 0.65, smile=0.2)]
    v, _ = run([faces, faces], cap)
    assert v.face_count == 2 and not v.all_smiling and not v.conditions_met


def test_zero_faces_is_false(cap: CaptureConfig) -> None:
    v, _ = run([[], []], cap)
    assert v.face_count == 0
    assert not (v.all_in_frame or v.all_eyes_open or v.all_smiling or v.conditions_met)
    assert v.hint is None


def test_count_unstable_holds(cap: CaptureConfig) -> None:
    # 1 → 2 人に変わった直後は保留、次のフレームで安定して達成
    v, s = run([[face()], [face()], two_faces()], cap)
    assert not v.conditions_met
    assert v.target_face_count == 1  # 1 フレームだけの 2 人では目標は上がらない
    v, s = evaluate_frame(two_faces(), s, cap)
    assert v.conditions_met and v.target_face_count == 2


def test_target_from_compose_max_and_below_target_rejected(cap: CaptureConfig) -> None:
    # COMPOSE で 2 人が安定 → 目標 2。その後 1 人に減っても下げず、1 人では採用しない
    v, s = run([two_faces(), two_faces(), [face()], [face()], [face()]], cap)
    assert v.target_face_count == 2
    assert v.face_count == 1 and not v.conditions_met


def test_target_increases_when_stable_more_faces(cap: CaptureConfig) -> None:
    s = CountState()
    for n in (1, 1, 2, 2):
        s = update_count(s, n, cap)
    assert s.target == 2
    s = update_count(s, 3, cap)
    assert s.target == 2  # 1 フレームだけでは上げない
    s = update_count(s, 1, cap)
    assert s.target == 2  # 減っても下げない


def test_five_faces_rejected_with_hint(cap: CaptureConfig) -> None:
    faces = [face(0.1 + i * 0.16, 0.35, 0.22 + i * 0.16, 0.65) for i in range(5)]
    v, _ = run([faces, faces], cap)
    assert v.face_count == 5
    assert v.hint == HINT_TOO_MANY and not v.conditions_met


def test_closer_hint_when_group_wider_than_safe_area(cap: CaptureConfig) -> None:
    faces = [face(0.02, 0.3, 0.2, 0.6), face(0.8, 0.3, 0.97, 0.6)]
    v, _ = run([faces], cap)
    assert v.hint == HINT_CLOSER


def test_closer_hint_from_head(cap: CaptureConfig) -> None:
    v, _ = evaluate_frame([face()], CountState(), cap, head_closer=True)
    assert v.hint == HINT_CLOSER


def test_accept_second_of_two_consecutive(cap: CaptureConfig) -> None:
    n, acc = accept_step(0, True, cap)
    assert (n, acc) == (1, False)
    n, acc = accept_step(n, True, cap)
    assert (n, acc) == (2, True)


def test_accept_resets_on_miss(cap: CaptureConfig) -> None:
    n, _ = accept_step(0, True, cap)
    n, acc = accept_step(n, False, cap)
    assert (n, acc) == (0, False)
    n, acc = accept_step(n, True, cap)
    assert not acc


def test_candidate_none_for_zero_faces(cap: CaptureConfig) -> None:
    assert candidate_score([], 100.0, cap) is None
    assert not is_better(None, None)


def test_timeout_ranking_lexicographic(cap: CaptureConfig) -> None:
    two_ok = candidate_score(two_faces(smile=0.3), 10.0, cap)
    one_perfect = candidate_score([face()], 999.0, cap)
    assert two_ok > one_perfect  # 人数が最優先

    cut = [face(0.01, 0.35, 0.2, 0.65, smile=0.9), face(0.6, 0.35, 0.8, 0.65, smile=0.9)]
    two_cut = candidate_score(cut, 999.0, cap)
    assert two_ok > two_cut  # 次に枠内人数

    closed = candidate_score(two_faces(blink=0.9, smile=0.9), 999.0, cap)
    assert two_ok > closed  # 次に開眼人数

    two_smile = candidate_score(two_faces(smile=0.5), 1.0, cap)
    assert two_smile > two_ok  # 次に最小笑顔スコア

    blurry = candidate_score(two_faces(smile=0.3), 5.0, cap)
    assert two_ok > blurry  # 最後にブレ

    assert is_better(two_smile, two_ok)
    assert not is_better(two_ok, two_ok)  # 同点は先着を残す
    assert is_better(two_ok, None)


def test_candidate_reason_and_scalar() -> None:
    s = CandidateScore(2, 2, 1, 0.314, 812.4)
    assert s.reason() == "faces=2 in_frame=2 eyes_open=1 min_smile=0.31 sharpness=812"
    assert s.scalar() == pytest.approx(2210.314)
    assert CandidateScore(2, 2, 2, 0.0, 0).scalar() > s.scalar()
