"""MediaPipeAnalyzer の手動テスト (pytest 対象外。モデルファイルが必要)。

使い方:
    uv run python tests/manual_mediapipe.py              # 合成画像: 顔 0 で例外が出ないこと
    uv run python tests/manual_mediapipe.py face.jpg ... # 手元の写真: 顔数と blendshape を表示

顔写真はコミットしない (spec §11)。
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import numpy as np

from edge.analysis import MediaPipeAnalyzer
from edge.config import load_config
from edge.decision import eyes_open, face_in_frame, smiling
from edge.image import decode_jpeg


def main(paths: list[str]) -> int:
    cfg = load_config()
    t0 = time.perf_counter()
    analyzer = MediaPipeAnalyzer(cfg.model_file(), cfg.capture.max_faces)
    print(f"model loaded in {time.perf_counter() - t0:.2f}s: {cfg.model_file()}")

    rng = np.random.default_rng(0)
    synthetic = [
        np.zeros((240, 320, 3), dtype=np.uint8),
        rng.integers(0, 256, (240, 320, 3), dtype=np.uint8),
        np.full((480, 640, 3), 200, dtype=np.uint8),  # 解像度が変わっても動くこと
    ]
    ts = 1
    for img in synthetic:
        t0 = time.perf_counter()
        faces = analyzer.analyze(img, ts)
        ms = (time.perf_counter() - t0) * 1000
        print(f"synthetic {img.shape[1]}x{img.shape[0]}: faces={len(faces)} ({ms:.1f} ms)")
        assert faces == [], "no faces expected in a synthetic image"
        ts += 100
    # 同じ・古いタイムスタンプでも例外にならない (内部で単調増加に繰り上げる)
    analyzer.analyze(synthetic[0], 0)
    analyzer.analyze(synthetic[0], 0)
    print("non-monotonic timestamps: ok")

    for p in paths:
        rgb = decode_jpeg(Path(p).read_bytes())
        ts += 100
        faces = analyzer.analyze(rgb, ts)
        print(f"{p}: faces={len(faces)}")
        for i, f in enumerate(faces):
            print(
                f"  [{i}] bbox={tuple(round(v, 3) for v in f.bbox)} "
                f"blink=({f.eye_blink_left:.2f},{f.eye_blink_right:.2f}) "
                f"smile=({f.mouth_smile_left:.2f},{f.mouth_smile_right:.2f}) "
                f"in_frame={face_in_frame(f, cfg.capture)} open={eyes_open(f, cfg.capture)} "
                f"smiling={smiling(f, cfg.capture)}"
            )
    analyzer.close()
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
