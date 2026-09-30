from __future__ import annotations

import io

import numpy as np
import pytest
from PIL import Image, ImageFilter

from edge.image import (
    ImageDecodeError,
    decode_frame,
    decode_jpeg,
    decode_rgb565,
    encode_jpeg,
    encode_rgb565,
    sharpness,
)

# 2x2: 赤 / 緑 / 青 / 白 (RGB565 の値)
PIXELS_565 = [0xF800, 0x07E0, 0x001F, 0xFFFF]
EXPECTED = np.array(
    [[[255, 0, 0], [0, 255, 0]], [[0, 0, 255], [255, 255, 255]]],
    dtype=np.uint8,
)


def _bytes(order: str) -> bytes:
    return b"".join(v.to_bytes(2, order) for v in PIXELS_565)  # type: ignore[arg-type]


@pytest.mark.parametrize("order", ["little", "big"])
def test_rgb565_known_pixels(order: str) -> None:
    rgb = decode_rgb565(_bytes(order), 2, 2, order)  # type: ignore[arg-type]
    assert rgb.dtype == np.uint8
    assert rgb.shape == (2, 2, 3)
    np.testing.assert_array_equal(rgb, EXPECTED)


def test_rgb565_wrong_byte_order_swaps_colors() -> None:
    # little で送られたものを big として読むと色が化ける (設定の切り替えが効くこと)
    rgb = decode_rgb565(_bytes("little"), 2, 2, "big")
    assert not np.array_equal(rgb, EXPECTED)


def test_rgb565_mid_values_expand_to_full_range() -> None:
    # R=16 (5bit), G=32 (6bit), B=16 → 132, 130, 132
    v = (16 << 11) | (32 << 5) | 16
    rgb = decode_rgb565(v.to_bytes(2, "little"), 1, 1, "little")
    assert rgb[0, 0].tolist() == [132, 130, 132]


@pytest.mark.parametrize("order", ["little", "big"])
def test_rgb565_roundtrip(order: str) -> None:
    rng = np.random.default_rng(1)
    src = rng.integers(0, 256, (6, 8, 3), dtype=np.uint8)
    back = decode_rgb565(encode_rgb565(src, order), 8, 6, order)  # type: ignore[arg-type]
    # 量子化誤差は R/B 8 未満、G 4 未満
    diff = np.abs(back.astype(int) - src.astype(int))
    assert diff[..., 0].max() < 8 and diff[..., 1].max() < 4 and diff[..., 2].max() < 8


def test_rgb565_length_mismatch() -> None:
    with pytest.raises(ImageDecodeError):
        decode_rgb565(b"\x00" * 7, 2, 2, "little")


def test_jpeg_roundtrip() -> None:
    src = np.zeros((24, 32, 3), dtype=np.uint8)
    src[:, :16] = [200, 50, 50]
    src[:, 16:] = [50, 50, 200]
    data = encode_jpeg(src, 90)
    assert data[:2] == b"\xff\xd8"
    back = decode_jpeg(data)
    assert back.shape == src.shape
    assert np.abs(back.astype(int) - src.astype(int)).mean() < 6


def test_encode_jpeg_has_no_exif() -> None:
    data = encode_jpeg(np.zeros((8, 8, 3), dtype=np.uint8))
    with Image.open(io.BytesIO(data)) as im:
        assert not im.getexif()


def test_decode_frame_dispatch_and_errors() -> None:
    assert decode_frame("rgb565", _bytes("big"), 2, 2, "big").shape == (2, 2, 3)
    with pytest.raises(ImageDecodeError):
        decode_frame("png", b"x", 1, 1, "little")
    with pytest.raises(ImageDecodeError):
        decode_jpeg(b"not a jpeg at all")
    png = io.BytesIO()
    Image.new("RGB", (4, 4)).save(png, format="PNG")
    with pytest.raises(ImageDecodeError):
        decode_jpeg(png.getvalue())


def test_sharpness_sharp_greater_than_blurred() -> None:
    # 市松模様 (鮮明) とそのぼかし
    tile = (np.indices((64, 64)).sum(axis=0) // 4 % 2 * 255).astype(np.uint8)
    sharp = np.stack([tile] * 3, axis=-1)
    blurred = np.asarray(Image.fromarray(sharp).filter(ImageFilter.GaussianBlur(radius=3)))
    assert sharpness(sharp) > sharpness(blurred) * 5
    assert sharpness(np.zeros((1, 1, 3), dtype=np.uint8)) == 0.0
