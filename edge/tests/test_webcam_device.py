"""tools/webcam_device.py の判定部分 (カメラ・GUI は使わない)。"""

from __future__ import annotations

import sys
from pathlib import Path

import httpx
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


# ---- 計測 (--stats / --candidate-via-edge) ----


def test_percentile_nearest_rank() -> None:
    values = [float(v) for v in range(1, 101)]
    assert wd.percentile(values, 50) == 50.0
    assert wd.percentile(values, 95) == 95.0
    assert wd.percentile([7.0], 95) == 7.0


def test_summarize_empty_and_counts() -> None:
    assert wd.summarize([]) == {"count": 0}
    s = wd.summarize([10.0, 20.0, 30.0])
    assert s["count"] == 3 and s["p50"] == 20.0 and s["max"] == 30.0


@pytest.mark.parametrize(
    ("headers", "expected"),
    [
        ({"transfer-encoding": "chunked"}, "chunked"),
        ({"content-length": "12"}, "content-length"),
        ({}, "none"),
    ],
)
def test_body_framing(headers, expected) -> None:
    assert wd.body_framing(httpx.Headers(headers)) == expected


def _client(handler, stats):
    return wd.EdgeClient(
        "https://edge.test", "dev", "k", stats=stats, transport=httpx.MockTransport(handler)
    )


def test_candidate_records_rtt_bytes_and_framing() -> None:
    stats = wd.Stats()
    edge = _client(lambda req: httpx.Response(200, content=b"\xff\xd8abc\xff\xd9"), stats)
    assert edge.candidate("sid") == b"\xff\xd8abc\xff\xd9"
    out = stats.to_json()["candidate"]
    assert out["rtt_ms"]["count"] == 1
    assert out["bytes"]["max"] == 7.0
    assert out["framing"] == {"content-length": 1}


def test_errors_are_counted_by_status_and_code() -> None:
    stats = wd.Stats()
    edge = _client(lambda req: httpx.Response(404, json={"error": "unknown_session"}), stats)
    with pytest.raises(wd.EdgeHTTPError) as info:
        edge.hello()
    assert info.value.code == "unknown_session"
    assert stats.to_json()["hello"]["errors"] == {"404:unknown_session": 1}


def test_wait_ready_retries_until_200(monkeypatch) -> None:
    monkeypatch.setattr(wd.time, "sleep", lambda _s: None)
    replies = iter([500, 503, 200])

    def handler(req):
        status = next(replies)
        if status == 200:
            return httpx.Response(200, json={"ready": True, "countdown_sec": 10})
        return httpx.Response(status, json={"error": "x"})

    stats = wd.Stats()
    info = wd.wait_ready(_client(handler, stats), max_sec=60, stats=stats)
    assert info["ready"] is True
    assert stats.cold_hello["attempts"] == 3
    assert stats.cold_hello["codes"] == ["500:x", "503:x", "200"]
    assert stats.hello_rtt_ms == []  # cold の hello は warm に混ぜない


def test_stats_json_has_no_secrets() -> None:
    stats = wd.Stats()
    edge = _client(lambda req: httpx.Response(200, json={"ready": True}), stats)
    edge.hello()
    text = str(stats.to_json({"edge_host": "edge.test"}))
    assert "X-Device-Key" not in text and "'k'" not in text
