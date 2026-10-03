"""保存する写真の四隅にｽﾀｯｸﾁｬﾝのドット絵を載せる。

素材と権利表記は assets/README.md を参照 (MIT License、ｽﾀｯｸﾁｬﾝ二次創作ガイドライン)。
四隅のどこかをランダムに選び、写真の内側を向くように左右を反転する
(元絵はどれも少し右を向いている)。下の角は見上げている絵、上の角は正面寄りの絵を使う。
"""

from __future__ import annotations

import random
from functools import cache
from importlib import resources
from typing import TYPE_CHECKING, Literal

import numpy as np
from PIL import Image, ImageFilter, ImageOps

if TYPE_CHECKING:
    from edge.config import StickerConfig

Corner = Literal["top_left", "top_right", "bottom_left", "bottom_right"]
CORNERS: tuple[Corner, ...] = ("top_left", "top_right", "bottom_left", "bottom_right")

_CELL = 32  # スプライトシート 1 コマの大きさ
_FRAME_FRONT = 0  # 正面寄り (上の角)
_FRAME_LOOK_UP = 2  # 少し見上げている (下の角)


@cache
def _sheet() -> Image.Image:
    with resources.files("edge").joinpath("assets", "stack-chan.png").open("rb") as f:
        return Image.open(f).convert("RGBA")


def _frame(index: int) -> Image.Image:
    row, col = divmod(index, 4)
    cell = _sheet().crop((col * _CELL, row * _CELL, (col + 1) * _CELL, (row + 1) * _CELL))
    box = cell.getbbox()
    return cell.crop(box) if box else cell


@cache
def sticker_image(corner: Corner, scale: int, outline: int) -> Image.Image:
    """角ごとの絵 (拡大 + 白い縁取り) を作る。結果はキャッシュする。"""
    frame = _frame(_FRAME_LOOK_UP if corner.startswith("bottom") else _FRAME_FRONT)
    if corner.endswith("right"):
        frame = ImageOps.mirror(frame)  # 右の角では左を向かせる
    big = frame.resize((frame.width * scale, frame.height * scale), Image.NEAREST)
    pad = outline + 1
    canvas = Image.new("RGBA", (big.width + 2 * pad, big.height + 2 * pad), (0, 0, 0, 0))
    if outline > 0:
        alpha = Image.new("L", canvas.size, 0)
        alpha.paste(big.split()[3], (pad, pad))
        ring = alpha.filter(ImageFilter.MaxFilter(2 * outline + 1))
        white = Image.new("RGBA", canvas.size, (255, 255, 255, 255))
        white.putalpha(ring)
        canvas.alpha_composite(white)
    canvas.alpha_composite(big, (pad, pad))
    return canvas


def apply_sticker(
    rgb: np.ndarray,
    cfg: StickerConfig,
    corner: Corner | None = None,
    rng: random.Random | None = None,
) -> np.ndarray:
    """rgb (H, W, 3 の uint8) の四隅のどれかにドット絵を重ねた新しい配列を返す。元の配列は変えない。

    corner を省略するとランダムに選ぶ。写真が小さすぎて絵が収まらないときは何もしない。
    """
    if not cfg.enabled:
        return rgb
    if corner is None:
        corner = (rng or random).choice(CORNERS)
    st = sticker_image(corner, cfg.scale, cfg.outline)
    h, w = rgb.shape[:2]
    m = cfg.margin
    if st.width + 2 * m > w or st.height + 2 * m > h:
        return rgb
    x = m if corner.endswith("left") else w - m - st.width
    y = m if corner.startswith("top") else h - m - st.height
    photo = Image.fromarray(rgb, "RGB").convert("RGBA")
    photo.alpha_composite(st, (x, y))
    return np.asarray(photo.convert("RGB"))
