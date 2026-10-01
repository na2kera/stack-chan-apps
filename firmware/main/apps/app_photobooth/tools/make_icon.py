#!/usr/bin/env python3
"""ランチャー用アイコン assets/icon_photobooth.c を作る。

純正アイコン (main/assets/assets_bin/icon_*.bin) と同じ 188x150 / LV_COLOR_FORMAT_RGB565A8 の
LVGL C 配列として出力する (assets パーティションには入れない)。図柄は単純なカメラ。

  python3 tools/make_icon.py [preview.png]

依存: Pillow
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 188, 150
SS = 4  # スーパーサンプリング倍率 (縁を滑らかにする)

OUTLINE = (20, 20, 20, 255)
BODY = (246, 246, 246, 255)
LENS_RING = (58, 58, 58, 255)
LENS_GLASS = (84, 140, 230, 255)
HIGHLIGHT = (255, 255, 255, 230)
FLASH = (255, 216, 77, 255)
SHUTTER = (255, 96, 96, 255)


def s(*v):
    return [int(round(x * SS)) for x in v]


def draw() -> Image.Image:
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = 6 * SS
    # ファインダーの出っ張り (本体より先に描いて下側を本体で隠す)
    d.rounded_rectangle(s(56, 24, 104, 52), radius=8 * SS, fill=BODY, outline=OUTLINE, width=lw)
    # シャッターボタン
    d.rounded_rectangle(s(126, 32, 154, 50), radius=5 * SS, fill=SHUTTER, outline=OUTLINE, width=4 * SS)
    # 本体
    d.rounded_rectangle(s(20, 42, 168, 132), radius=18 * SS, fill=BODY, outline=OUTLINE, width=lw)
    # フラッシュ窓
    d.rounded_rectangle(s(132, 54, 154, 68), radius=3 * SS, fill=FLASH, outline=OUTLINE, width=3 * SS)
    # レンズ
    cx, cy = 92, 88
    d.ellipse(s(cx - 34, cy - 34, cx + 34, cy + 34), fill=LENS_RING, outline=OUTLINE, width=lw)
    d.ellipse(s(cx - 21, cy - 21, cx + 21, cy + 21), fill=LENS_GLASS, outline=OUTLINE, width=3 * SS)
    d.ellipse(s(cx - 12, cy - 13, cx - 3, cy - 4), fill=HIGHLIGHT)
    return img.resize((W, H), Image.LANCZOS)


def to_rgb565a8(img: Image.Image) -> bytes:
    rgb = bytearray()
    alpha = bytearray()
    px = img.tobytes()  # RGBA 8bit x4
    for i in range(0, len(px), 4):
        r, g, b, a = px[i : i + 4]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        rgb += bytes((v & 0xFF, v >> 8))  # リトルエンディアン (純正の .bin と同じ)
        alpha.append(a)
    return bytes(rgb + alpha)


def main() -> None:
    img = draw()
    if len(sys.argv) > 1:
        bg = Image.new("RGBA", (W, H), (255, 204, 51, 255))
        bg.alpha_composite(img)
        bg.save(sys.argv[1])
    data = to_rgb565a8(img)
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 24]) + ",")
    out = Path(__file__).resolve().parent.parent / "assets" / "icon_photobooth.c"
    out.write_text(
        f"""/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// ランチャー用アイコン ({W}x{H}, RGB565A8)。tools/make_icon.py で生成。手で編集しない。
#include <lvgl.h>

static const uint8_t icon_photobooth_map[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t icon_photobooth = {{
    .header =
        {{
            .magic  = LV_IMAGE_HEADER_MAGIC,
            .cf     = LV_COLOR_FORMAT_RGB565A8,
            .flags  = 0,
            .w      = {W},
            .h      = {H},
            .stride = {W * 2},
        }},
    .data_size = sizeof(icon_photobooth_map),
    .data      = icon_photobooth_map,
}};
""",
        encoding="utf-8",
    )
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
