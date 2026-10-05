#!/usr/bin/env python3
"""リールのシート assets/rl_reel.c を作る (docs/design/app-roulette.md §5)。

assets/src/*.svg (技育展・技育祭・技育博・技育CAMP のロゴ) を rsvg-convert でラスタライズし、
1 シンボル 72x72 を縦に 4 つ並べた 72x288 の不透明な画像にして、LVGL の RGB565 C 配列で出力する。
並び (上から 0=技育展, 1=技育祭, 2=技育博, 3=技育CAMP) は view/strings.h の kSymbolNames と揃えること。

見え方は移植元 (stack-chan-roulette の tools/build-assets.sh) と同じ: 暗い下地 #0d0d12 に、
セルの 56/60 の大きさのロゴを中央に置く。ここでは 72px のセルに 67px のロゴ。

  python3 tools/make_reel.py [preview.png]

依存: rsvg-convert (librsvg), magick (ImageMagick)。Python は標準ライブラリだけ。
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SYMBOL = 72
LOGO = 67  # 移植元 56/60 の比率
BG = "#0d0d12"
ORDER = ["geekten", "geeksai", "geekhaku", "geekcamp"]
W, H = SYMBOL, SYMBOL * len(ORDER)

APP_DIR = Path(__file__).resolve().parent.parent
SRC = APP_DIR / "assets" / "src"
OUT = APP_DIR / "assets" / "rl_reel.c"


def run(*args: str) -> bytes:
    return subprocess.run(args, check=True, stdout=subprocess.PIPE).stdout


def build_sheet(tmp: Path) -> Path:
    parts = []
    for name in ORDER:
        svg = SRC / f"{name}.svg"
        if not svg.is_file():
            sys.exit(f"not found: {svg}")
        logo = tmp / f"{name}.png"
        cell = tmp / f"cell-{name}.png"
        run("rsvg-convert", "-w", str(LOGO), "-h", str(LOGO), "-a", str(svg), "-o", str(logo))
        run("magick", str(logo), "-background", BG, "-alpha", "remove", "-alpha", "off",
            "-gravity", "center", "-extent", f"{SYMBOL}x{SYMBOL}", str(cell))
        parts.append(str(cell))
    sheet = tmp / "sheet.png"
    run("magick", *parts, "-append", "-alpha", "off", str(sheet))
    return sheet


def to_rgb565(rgb: bytes) -> bytes:
    out = bytearray()
    for i in range(0, len(rgb), 3):
        r, g, b = rgb[i], rgb[i + 1], rgb[i + 2]
        v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        out += bytes((v & 0xFF, v >> 8))  # リトルエンディアン
    return bytes(out)


def main() -> None:
    for tool in ("rsvg-convert", "magick"):
        if shutil.which(tool) is None:
            sys.exit(f"{tool} is required")
    with tempfile.TemporaryDirectory() as d:
        sheet = build_sheet(Path(d))
        if len(sys.argv) > 1:
            shutil.copyfile(sheet, sys.argv[1])
        rgb = run("magick", str(sheet), "-depth", "8", "rgb:-")
    if len(rgb) != W * H * 3:
        sys.exit(f"unexpected size: {len(rgb)} bytes (expected {W * H * 3})")
    data = to_rgb565(rgb)
    lines = []
    for i in range(0, len(data), 24):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in data[i : i + 24]) + ",")
    OUT.write_text(
        f"""/*
 * SPDX-FileCopyrightText: 2026 na2kera
 *
 * SPDX-License-Identifier: MIT
 */
// リールのシート ({W}x{H}, RGB565)。上から 技育展 / 技育祭 / 技育博 / 技育CAMP。
// tools/make_reel.py で assets/src/*.svg から生成。手で編集しない。
#include <lvgl.h>

static const uint8_t rl_reel_map[] = {{
{chr(10).join(lines)}
}};

const lv_image_dsc_t rl_reel = {{
    .header =
        {{
            .magic  = LV_IMAGE_HEADER_MAGIC,
            .cf     = LV_COLOR_FORMAT_RGB565,
            .flags  = 0,
            .w      = {W},
            .h      = {H},
            .stride = {W * 2},
        }},
    .data_size = sizeof(rl_reel_map),
    .data      = rl_reel_map,
}};
""",
        encoding="utf-8",
    )
    print(f"wrote {OUT} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
