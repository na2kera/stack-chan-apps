from __future__ import annotations

from dataclasses import replace

import pytest
from conftest import face

from edge.config import Config, HeadConfig
from edge.head import SEARCH_PATTERN, SEARCH_ROUND_TRIPS, HeadState, compute_head

W, H = 320, 240
MARGIN = 0.08


@pytest.fixture
def head(cfg: Config) -> HeadConfig:
    return cfg.head


def step(faces, hc, *, sx=0, sy=450, phase="capture", now=10_000, state=None):
    return compute_head(faces, W, H, sx, sy, phase, now, state or HeadState(), hc, MARGIN)


def centered_at(cx: float, cy: float = 0.5, w: float = 0.2, h: float = 0.3):
    return face(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2)


def test_deadband(head: HeadConfig) -> None:
    # 中心から 16px 以内は動かさない (0.05 * 320 = 16px)
    cmd, st = step([centered_at(0.5 + 16 / W, 0.5 + 10 / H)], head)
    assert (cmd.dx, cmd.dy) == (0, 0)
    assert st.last_command_ms is None
    cmd, _ = step([centered_at(0.5 + 20 / W)], head)
    assert cmd.dx != 0


def test_gain_and_sign(head: HeadConfig) -> None:
    # 右に 80px ずれ → dx = 80 * -0.05 = -4、下に 40px → dy = 40 * 0.05 = 2
    cmd, st = step([centered_at(0.75, 0.5 + 40 / H)], head)
    assert (cmd.dx, cmd.dy) == (-4, 2)
    assert st.last_command_ms == 10_000
    cmd, _ = step([centered_at(0.25)], head)
    assert cmd.dx == 4  # 左右で符号が変わる
    flipped = replace(head, gain_x=0.05)
    cmd, _ = step([centered_at(0.75)], flipped)
    assert cmd.dx == 4  # 符号は設定で校正できる


def test_step_max(head: HeadConfig) -> None:
    big = replace(head, gain_x=-1.0, gain_y=1.0)
    cmd, _ = step([centered_at(0.8, 0.8)], big)
    assert (cmd.dx, cmd.dy) == (-30, 30)
    assert abs(cmd.dx) <= head.step_max


def test_min_interval(head: HeadConfig) -> None:
    f = [centered_at(0.75)]
    cmd, st = step(f, head, now=1000)
    assert cmd.dx != 0
    cmd, st2 = step(f, head, now=1499, state=st)
    assert (cmd.dx, cmd.dy) == (0, 0)
    assert st2 == st
    cmd, st3 = step(f, head, now=1500, state=st2)
    assert cmd.dx != 0 and st3.last_command_ms == 1500


def test_group_center_uses_union_bbox(head: HeadConfig) -> None:
    # 左右に 1 人ずつ、外接矩形の中心は画像中心 → 動かさない
    faces = [face(0.15, 0.35, 0.35, 0.65), face(0.65, 0.35, 0.85, 0.65)]
    cmd, _ = step(faces, head)
    assert (cmd.dx, cmd.dy) == (0, 0)


def test_cut_side_is_prioritized_over_deadband(head: HeadConfig) -> None:
    # 外接矩形の中心は画像中心に近い (デッドバンド内) が、左端の顔が余白を割っている
    faces = [face(0.05, 0.35, 0.25, 0.65), face(0.6, 0.35, 0.9, 0.65)]
    big_gain = replace(head, gain_x=-0.5)
    cmd, _ = step(faces, big_gain)
    # 左の見切れ量 = 0.05*320 - 0.08*320 = -9.6px → dx = -9.6 * -0.5 = 5 (左へ寄せる向き)
    assert cmd.dx == 5


def test_search_only_in_compose_and_limited_round_trips(head: HeadConfig) -> None:
    cmd, st = step([], head, phase="capture")
    assert (cmd.dx, cmd.dy) == (0, 0)

    moves = []
    st = HeadState()
    now = 0
    for _ in range(20):
        now += 500
        cmd, st = step([], head, phase="compose", now=now, state=st)
        moves.append(cmd.dx)
    nonzero = [m for m in moves if m]
    assert nonzero == [p * head.search_step for p in SEARCH_PATTERN] * SEARCH_ROUND_TRIPS
    assert sum(nonzero) == 0  # 探索後は元の位置に戻る
    assert moves[len(nonzero) :] == [0] * (20 - len(nonzero))


def test_search_respects_interval(head: HeadConfig) -> None:
    cmd, st = step([], head, phase="compose", now=1000)
    assert cmd.dx == head.search_step
    cmd, st2 = step([], head, phase="compose", now=1200, state=st)
    assert cmd.dx == 0 and st2.search_moves == 1


def test_range_clamp(head: HeadConfig) -> None:
    big = replace(head, gain_x=-1.0)
    # 右にずれ → dx は負。x_min の 10 手前なら -10 までしか動かさない
    cmd, _ = step([centered_at(0.75)], big, sx=head.x_min + 10)
    assert cmd.dx == -10


def test_closer_when_at_limit_and_still_cut(head: HeadConfig) -> None:
    # 顔が右端で見切れ、首はすでに x_min (右へ寄せる向きの端) → 動かさず closer
    cut_right = [face(0.8, 0.35, 0.99, 0.65)]
    cmd, _ = step(cut_right, head, sx=head.x_min)
    assert cmd.dx == 0 and cmd.closer
    # 端でなければ closer ではない
    cmd, _ = step(cut_right, head, sx=0)
    assert cmd.dx < 0 and not cmd.closer
    # 端にいても見切れていなければ (中心合わせだけなら) closer ではない
    cmd, _ = step([centered_at(0.75)], head, sx=head.x_min)
    assert cmd.dx == 0 and not cmd.closer


def test_closer_vertical_limit(head: HeadConfig) -> None:
    cut_bottom = [face(0.4, 0.7, 0.6, 0.99)]
    cmd, _ = step(cut_bottom, head, sy=head.y_max)
    assert cmd.dy == 0 and cmd.closer


def test_interval_not_bypassed_by_clock_going_back(head: HeadConfig) -> None:
    f = [centered_at(0.75)]
    cmd, st = step(f, head, now=10_000)
    assert cmd.dx != 0
    cmd, _ = step(f, head, now=0, state=st)  # 時計が戻っても間隔制限は外れない
    assert (cmd.dx, cmd.dy) == (0, 0)
