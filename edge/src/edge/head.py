"""首振り量の純関数 (spec §6.3 / design §6)。

単位: 画像は px、サーボは 1/10 度。servo_dx = image_dx * gain_x (符号は設定で校正する)。
FastAPI・mediapipe・numpy に依存しない。
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING, Literal

if TYPE_CHECKING:
    from collections.abc import Sequence

    from edge.analysis import FaceObservation
    from edge.config import HeadConfig

# 探索の 1 往復: 右へ 1 段 → 戻る → 左へ 1 段 → 戻る (servo_dx の符号で表す)
SEARCH_PATTERN: tuple[int, ...] = (1, -1, -1, 1)
SEARCH_ROUND_TRIPS = 2


@dataclass(frozen=True)
class HeadState:
    last_command_ms: int | None = None  # 最後に 0 以外を返したときの edge の単調時計 (ms)
    search_moves: int = 0  # 探索で出した手数


@dataclass(frozen=True)
class HeadCommand:
    dx: int
    dy: int
    closer: bool  # 可動域端に達していて、まだ同じ方向へ寄せたい (見切れ補正が効かない)


@dataclass(frozen=True)
class _Axis:
    error_px: float  # 動かしたい量 (画像上の px、中心からのずれ)
    one_side_cut: bool  # 片側だけ margin を割っている (補正を優先)


def _clamp(v: int, lo: int, hi: int) -> int:
    return max(lo, min(hi, v))


def _axis(lo_px: float, hi_px: float, size: int, margin_px: float) -> _Axis:
    center = (lo_px + hi_px) / 2.0 - size / 2.0
    cut_lo = lo_px < margin_px
    cut_hi = hi_px > size - margin_px
    if cut_lo and not cut_hi:
        # 顔群が低い側 (左/上) で見切れ: その方向へ、中心ずれと見切れ量の大きい方だけ寄せる
        return _Axis(min(center, lo_px - margin_px), True)
    if cut_hi and not cut_lo:
        return _Axis(max(center, hi_px - (size - margin_px)), True)
    return _Axis(center, False)


def _command(axis: _Axis, gain: float, cfg: HeadConfig) -> int:
    if not axis.one_side_cut and abs(axis.error_px) <= cfg.deadband_px:
        return 0
    return _clamp(round(axis.error_px * gain), -cfg.step_max, cfg.step_max)


def _limit(cmd: int, pos: int, lo: int, hi: int) -> tuple[int, bool]:
    """可動域の端で同じ方向へ押そうとしていたら 0 にする。戻り値の bool は「端で止められた」。"""
    if cmd > 0 and pos >= hi:
        return 0, True
    if cmd < 0 and pos <= lo:
        return 0, True
    return _clamp(pos + cmd, lo, hi) - pos, False


def compute_head(
    faces: Sequence[FaceObservation],
    width: int,
    height: int,
    servo_x: int,
    servo_y: int,
    phase: Literal["compose", "capture"],
    now_ms: int,
    state: HeadState,
    cfg: HeadConfig,
    margin_ratio: float,
) -> tuple[HeadCommand, HeadState]:
    """1 フレーム分の首振り量を計算する。

    - 顔あり: 全顔の外接矩形の中心と画像中心の差をデッドバンド・ゲイン・上限で変換。
      片側だけ margin を割っている軸は、その方向への補正を優先しデッドバンドを掛けない。
    - 顔なし: COMPOSE のときだけ search_step で左右交互に探索 (往復 2 回まで)。
    - 前回の指示から min_interval_ms 未満なら 0 (now_ms は edge の単調時計)。
    """
    # now_ms は edge の単調時計。device の capture_ms は使わない (戻されると制限を外せてしまう)
    interval_ok = (
        state.last_command_ms is None or now_ms - state.last_command_ms >= cfg.min_interval_ms
    )

    if faces:
        x0 = min(f.bbox[0] for f in faces) * width
        y0 = min(f.bbox[1] for f in faces) * height
        x1 = max(f.bbox[2] for f in faces) * width
        y1 = max(f.bbox[3] for f in faces) * height
        ax = _axis(x0, x1, width, margin_ratio * width)
        ay = _axis(y0, y1, height, margin_ratio * height)
        dx, stuck_x = _limit(_command(ax, cfg.gain_x, cfg), servo_x, cfg.x_min, cfg.x_max)
        dy, stuck_y = _limit(_command(ay, cfg.gain_y, cfg), servo_y, cfg.y_min, cfg.y_max)
        closer = (stuck_x and ax.one_side_cut) or (stuck_y and ay.one_side_cut)
        search_moves = state.search_moves
    elif phase == "compose" and state.search_moves < len(SEARCH_PATTERN) * SEARCH_ROUND_TRIPS:
        step = SEARCH_PATTERN[state.search_moves % len(SEARCH_PATTERN)] * cfg.search_step
        dx, _ = _limit(step, servo_x, cfg.x_min, cfg.x_max)
        dy, closer = 0, False
        search_moves = state.search_moves + 1  # 端で動けなくてもこの手は消費する
    else:
        return HeadCommand(0, 0, False), state

    if not interval_ok:
        return HeadCommand(0, 0, closer), state
    if dx == 0 and dy == 0:
        return HeadCommand(0, 0, closer), HeadState(state.last_command_ms, search_moves)
    return HeadCommand(dx, dy, closer), HeadState(now_ms, search_moves)
