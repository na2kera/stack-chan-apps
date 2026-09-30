"""MediaPipe Face Landmarker のモデル (float16) を edge/models/ に取得する。

使い方: uv run python tools/download_model.py [--force]
"""

from __future__ import annotations

import argparse
import sys
import urllib.request
from pathlib import Path

MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/face_landmarker/"
    "face_landmarker/float16/1/face_landmarker.task"
)
EDGE_DIR = Path(__file__).resolve().parent.parent
DEFAULT_DEST = EDGE_DIR / "models" / "face_landmarker.task"


def download(dest: Path, force: bool = False) -> Path:
    if dest.exists() and not force:
        print(f"already exists: {dest} ({dest.stat().st_size} bytes). --force で取り直す")
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    print(f"downloading {MODEL_URL}")
    with urllib.request.urlopen(MODEL_URL, timeout=60) as resp, tmp.open("wb") as f:
        while chunk := resp.read(1 << 16):
            f.write(chunk)
    if tmp.stat().st_size < 1024:
        tmp.unlink()
        raise RuntimeError("downloaded file is too small; check the URL / network")
    tmp.replace(dest)
    print(f"saved: {dest} ({dest.stat().st_size} bytes)")
    return dest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST)
    parser.add_argument("--force", action="store_true", help="既存ファイルを上書きする")
    args = parser.parse_args()
    try:
        download(args.dest, force=args.force)
    except Exception as exc:  # noqa: BLE001 - CLI の終了コードに変換する
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
