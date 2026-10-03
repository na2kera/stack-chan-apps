"""写真の四隅に載せるドット絵 (sticker.py)。"""

import random

import numpy as np
import pytest

from edge.config import StickerConfig
from edge.sticker import CORNERS, apply_sticker, sticker_image

W, H = 320, 240
CFG = StickerConfig()


def gray() -> np.ndarray:
    return np.full((H, W, 3), 100, dtype=np.uint8)


def changed_box(before: np.ndarray, after: np.ndarray) -> tuple[int, int, int, int]:
    ys, xs = np.nonzero((before != after).any(axis=2))
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


@pytest.mark.parametrize("corner", CORNERS)
def test_sticker_lands_in_the_requested_corner(corner) -> None:
    src = gray()
    out = apply_sticker(src, CFG, corner=corner)
    assert out.shape == src.shape and out.dtype == np.uint8
    assert (src == 100).all()  # 元の配列は変えない
    x0, y0, x1, y1 = changed_box(src, out)
    st = sticker_image(corner, CFG.scale, CFG.outline)
    left = corner.endswith("left")
    top = corner.startswith("top")
    assert (x0 >= CFG.margin) if left else (x1 <= W - 1 - CFG.margin)
    assert (x1 < W // 2) if left else (x0 >= W // 2)
    assert (y0 >= CFG.margin) if top else (y1 <= H - 1 - CFG.margin)
    assert (y1 < H // 2) if top else (y0 >= H // 2)
    assert x1 - x0 < st.width and y1 - y0 < st.height


def test_white_outline_is_drawn() -> None:
    out = apply_sticker(gray(), CFG, corner="bottom_left")
    assert ((out == 255).all(axis=2)).any()


def test_right_corners_are_mirrored() -> None:
    left = np.asarray(sticker_image("top_left", 2, 2))
    right = np.asarray(sticker_image("top_right", 2, 2))
    assert (left[:, ::-1] == right).all()


def test_random_corner_uses_all_four() -> None:
    rng = random.Random(1)
    seen = set()
    for _ in range(40):
        out = apply_sticker(gray(), CFG, rng=rng)
        x0, y0, _, _ = changed_box(gray(), out)
        seen.add((x0 < W // 2, y0 < H // 2))
    assert len(seen) == 4


def test_disabled_returns_input_unchanged() -> None:
    src = gray()
    assert apply_sticker(src, StickerConfig(enabled=False)) is src


def test_too_small_photo_is_left_alone() -> None:
    tiny = np.zeros((40, 40, 3), dtype=np.uint8)
    assert apply_sticker(tiny, CFG, corner="top_left") is tiny
