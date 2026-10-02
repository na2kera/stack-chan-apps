#!/usr/bin/env python3
"""合成のシャッター音 (カシャッ) を作る。外部素材を使わないので権利の心配が無い。

    python3 tools/make_shutter.py

出力: assets/voice/shutter.wav (24 kHz / mono / 16-bit。純正コーデックの出力レートに合わせる)
標準ライブラリだけで動く。乱数の種を固定しているので、何度作っても同じ音になる。
"""
import math
import random
import struct
import wave
from pathlib import Path

RATE = 24000
OUT = Path(__file__).resolve().parent.parent / "assets" / "voice" / "shutter.wav"


def click(t0: float, dur: float, gain: float, tone_hz: float, rnd: random.Random) -> list[tuple[int, float]]:
    """t0 秒から dur 秒の、ノイズ + 短い金属音の減衰。"""
    out = []
    n0, n = int(t0 * RATE), int(dur * RATE)
    prev = 0.0
    for i in range(n):
        t = i / RATE
        env = math.exp(-t / (dur / 5))  # 速く減衰
        noise = rnd.uniform(-1.0, 1.0)
        hp = noise - prev * 0.6  # 軽いハイパスで「シャッ」とした高域を出す
        prev = noise
        tone = math.sin(2 * math.pi * tone_hz * t) * math.exp(-t / (dur / 8))
        out.append((n0 + i, gain * env * (0.75 * hp + 0.25 * tone)))
    return out


def main() -> None:
    rnd = random.Random(20261003)
    total = int(0.24 * RATE)
    buf = [0.0] * total
    # 先幕 (カシャ) と後幕 (ッ) の 2 つのクリック
    for idx, v in click(0.000, 0.060, 0.9, 2400.0, rnd) + click(0.085, 0.110, 0.7, 1700.0, rnd):
        if idx < total:
            buf[idx] += v
    peak = max(abs(v) for v in buf) or 1.0
    frames = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v / peak * 0.85)) * 32767)) for v in buf)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(OUT), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)
    print(f"wrote {OUT} ({total} samples, {total * 1000 // RATE} ms)")


if __name__ == "__main__":
    main()
