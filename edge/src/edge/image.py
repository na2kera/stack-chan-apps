"""画像の変換: RGB565 / JPEG → RGB ndarray、JPEG エンコード、ブレ指標。

OpenCV は使わない (numpy + Pillow のみ)。RGB ndarray は (H, W, 3) uint8。
"""

from __future__ import annotations

import io
from typing import Literal

import numpy as np
from PIL import Image, UnidentifiedImageError

ByteOrder = Literal["little", "big"]

# 既定の上限 (config の [capture] max_width / max_height)。
# device は QVGA、webcam ツールも縮小して送る
DEFAULT_MAX_WIDTH = 1280
DEFAULT_MAX_HEIGHT = 960


class ImageDecodeError(ValueError):
    """フレームのバイト列が宣言と合わない、または壊れている。"""


class ImageTooLargeError(ImageDecodeError):
    """宣言サイズ、またはデコードしたサイズが上限を超えている。"""


def _check_size(width: int, height: int, max_width: int, max_height: int) -> None:
    if width <= 0 or height <= 0:
        raise ImageDecodeError(f"invalid size {width}x{height}")
    if width > max_width or height > max_height:
        raise ImageTooLargeError(f"{width}x{height} exceeds {max_width}x{max_height}")


def decode_rgb565(
    data: bytes,
    width: int,
    height: int,
    byte_order: ByteOrder,
    max_width: int = DEFAULT_MAX_WIDTH,
    max_height: int = DEFAULT_MAX_HEIGHT,
) -> np.ndarray:
    """RGB565 (1 画素 2 バイト) を RGB888 に展開する。

    byte_order="little" は下位バイトが先 (ESP32 のメモリ上の uint16 そのまま)、
    "big" は上位バイトが先 (esp_camera の RGB565 出力はこちらのことが多い)。実機で確認する。
    """
    _check_size(width, height, max_width, max_height)
    expected = width * height * 2
    if len(data) != expected:
        raise ImageDecodeError(f"rgb565 length {len(data)} != {expected} ({width}x{height})")
    dtype = "<u2" if byte_order == "little" else ">u2"
    px = np.frombuffer(data, dtype=dtype).reshape(height, width).astype(np.uint16)
    r5 = (px >> 11) & 0x1F
    g6 = (px >> 5) & 0x3F
    b5 = px & 0x1F
    rgb = np.empty((height, width, 3), dtype=np.uint8)
    rgb[..., 0] = ((r5 << 3) | (r5 >> 2)).astype(np.uint8)
    rgb[..., 1] = ((g6 << 2) | (g6 >> 4)).astype(np.uint8)
    rgb[..., 2] = ((b5 << 3) | (b5 >> 2)).astype(np.uint8)
    return rgb


def encode_rgb565(rgb: np.ndarray, byte_order: ByteOrder) -> bytes:
    """RGB888 → RGB565 バイト列。webcam ツールとテストで device と同じ形式を作るために使う。"""
    if rgb.ndim != 3 or rgb.shape[2] != 3:
        raise ValueError("rgb must be (H, W, 3)")
    c = rgb.astype(np.uint16)
    px = ((c[..., 0] >> 3) << 11) | ((c[..., 1] >> 2) << 5) | (c[..., 2] >> 3)
    dtype = "<u2" if byte_order == "little" else ">u2"
    return px.astype(dtype).tobytes()


def decode_jpeg(
    data: bytes, max_width: int = DEFAULT_MAX_WIDTH, max_height: int = DEFAULT_MAX_HEIGHT
) -> np.ndarray:
    try:
        with Image.open(io.BytesIO(data)) as im:
            if im.format != "JPEG":
                raise ImageDecodeError(f"not a JPEG ({im.format})")
            # 画素を展開する前にヘッダのサイズで判定する
            _check_size(im.width, im.height, max_width, max_height)
            # Exif の向きには依存しない (spec §6.1)。受け取った画素のまま扱う
            return np.asarray(im.convert("RGB"), dtype=np.uint8).copy()
    except (UnidentifiedImageError, OSError, SyntaxError) as exc:
        raise ImageDecodeError(f"broken jpeg: {type(exc).__name__}") from exc


def decode_frame(
    fmt: str,
    data: bytes,
    width: int,
    height: int,
    byte_order: ByteOrder,
    max_width: int = DEFAULT_MAX_WIDTH,
    max_height: int = DEFAULT_MAX_HEIGHT,
) -> np.ndarray:
    _check_size(width, height, max_width, max_height)  # 宣言サイズ (X-Width / X-Height)
    if fmt == "rgb565":
        return decode_rgb565(data, width, height, byte_order, max_width, max_height)
    if fmt == "jpeg":
        return decode_jpeg(data, max_width, max_height)
    raise ImageDecodeError(f"unsupported format {fmt!r}")


def encode_jpeg(rgb: np.ndarray, quality: int = 90) -> bytes:
    buf = io.BytesIO()
    # Exif を付けずに出す。画素がそのまま正立 (spec §6.1)
    Image.fromarray(np.ascontiguousarray(rgb, dtype=np.uint8)).save(
        buf, format="JPEG", quality=quality, optimize=True
    )
    return buf.getvalue()


def sharpness(rgb: np.ndarray) -> float:
    """ブレ指標: グレースケールの 4 近傍ラプラシアンの分散。大きいほど鮮明。"""
    if rgb.shape[0] < 3 or rgb.shape[1] < 3:
        return 0.0
    f = rgb.astype(np.float32)
    gray = 0.299 * f[..., 0] + 0.587 * f[..., 1] + 0.114 * f[..., 2]
    lap = (
        gray[:-2, 1:-1] + gray[2:, 1:-1] + gray[1:-1, :-2] + gray[1:-1, 2:] - 4.0 * gray[1:-1, 1:-1]
    )
    return float(lap.var())
