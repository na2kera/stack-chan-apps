#!/usr/bin/env python3
"""ランチャー用アイコン assets/icon_roulette.c を作る。

純正アイコン (main/assets/assets_bin/icon_*.bin) と同じ 188x150 / LV_COLOR_FORMAT_RGB565A8 の
LVGL C 配列として出力する (assets パーティションには入れない)。図柄は 3 リールのスロット台。
ランチャーの背景色 (app_roulette.cpp のテーマ色 0x6C5CE7) の上に載る。

  python3 tools/make_icon.py [preview.png]
  (Pillow が無ければ: uv run --with pillow python3 tools/make_icon.py)

依存: Pillow
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw

W, H = 188, 150
SS = 4  # スーパーサンプリング倍率 (縁を滑らかにする)
THEME = (0x6C, 0x5C, 0xE7, 255)  # プレビューの背景 (ランチャーの背景色)

OUTLINE = (20, 20, 20, 255)
BODY = (246, 246, 246, 255)
WINDOW_BG = (18, 18, 26, 255)  # 画面の背景 #12121a
PAYLINE = (255, 204, 51, 255)  # 当たりの色 #ffcc33
KNOB = (255, 96, 96, 255)
# 窓に出す丸 (ロゴの輪の色: 技育展の青・技育祭の赤・技育CAMP の橙)
SYMBOLS = [(62, 95, 190, 255), (214, 38, 38, 255), (240, 120, 30, 255)]


def s(*v):
    return [int(round(x * SS)) for x in v]


def draw() -> Image.Image:
    img = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    lw = 6 * SS
    # レバー (本体より先に描いて付け根を本体で隠す)
    d.line(s(160, 92, 172, 92), fill=OUTLINE, width=8 * SS)
    d.line(s(172, 92, 172, 42), fill=OUTLINE, width=8 * SS)
    d.ellipse(s(162, 26, 182, 46), fill=KNOB, outline=OUTLINE, width=4 * SS)
    # 本体
    d.rounded_rectangle(s(14, 22, 164, 134), radius=18 * SS, fill=BODY, outline=OUTLINE, width=lw)
    # 3 リールの窓
    d.rounded_rectangle(s(28, 44, 150, 112), radius=8 * SS, fill=WINDOW_BG, outline=OUTLINE, width=4 * SS)
    # ペイライン (丸の後ろ)
    d.line(s(30, 78, 148, 78), fill=PAYLINE, width=3 * SS)
    for i, color in enumerate(SYMBOLS):
        cx = 49 + i * 40
        cy = 78
        d.ellipse(s(cx - 15, cy - 15, cx + 15, cy + 15), fill=(255, 255, 255, 255), outline=color, width=6 * SS)
    # リールの仕切り
    for x in (69, 109):
        d.line(s(x, 46, x, 110), fill=OUTLINE, width=3 * SS)
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
        bg = Image.new("RGBA", (W, H), THEME)
        bg.alpha_composite(img)
        bg.save(sys.argv[1])
    data = to_rgb565a8(img)
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 24]) + ",")
    out = Path(__file__).resolve().parent.parent / "assets" / "icon_roulette.c"
    out.write_text(
        f"""/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// ランチャー用アイコン ({W}x{H}, RGB565A8)。tools/make_icon.py で生成。手で編集しない。
#include <lvgl.h>

static const uint8_t icon_roulette_map[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t icon_roulette = {{
    .header =
        {{
            .magic  = LV_IMAGE_HEADER_MAGIC,
            .cf     = LV_COLOR_FORMAT_RGB565A8,
            .flags  = 0,
            .w      = {W},
            .h      = {H},
            .stride = {W * 2},
        }},
    .data_size = sizeof(icon_roulette_map),
    .data      = icon_roulette_map,
}};
""",
        encoding="utf-8",
    )
    print(f"wrote {out} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
