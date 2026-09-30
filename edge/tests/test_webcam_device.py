"""tools/webcam_device.py の判定部分 (カメラ・GUI は使わない)。"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import webcam_device as wd  # noqa: E402


@pytest.mark.parametrize(
    ("res", "phase", "elapsed", "expected"),
    [
        ({"accepted": True}, "capture", 9.9, True),
        ({"accepted": True}, "capture", 10.0, False),  # 10 秒経過後に返った accepted は無視
        ({"accepted": True}, "capture", 12.5, False),
        ({"accepted": True}, "compose", 1.0, False),
        ({"accepted": False}, "capture", 1.0, False),
        (None, "capture", 1.0, False),
    ],
)
def test_accepted_only_before_deadline(res, phase, elapsed, expected) -> None:
    assert wd.accept_result(res, phase, elapsed, countdown_sec=10.0) is expected
