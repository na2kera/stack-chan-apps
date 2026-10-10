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


class FakeClock:
    """time.monotonic / time.sleep の代わり。hello のタイムアウト分だけ時間を進める。"""

    def __init__(self) -> None:
        self.now = 1000.0
        self.slept: list[float] = []

    def monotonic(self) -> float:
        return self.now

    def sleep(self, sec: float) -> None:
        self.slept.append(sec)
        self.now += max(0.0, sec)


def test_wait_ready_never_exceeds_max_sec(monkeypatch) -> None:
    clock = FakeClock()
    monkeypatch.setattr(wd.time, "monotonic", clock.monotonic)
    monkeypatch.setattr(wd.time, "sleep", clock.sleep)
    timeouts: list[float] = []

    def handler(req):
        read = req.extensions["timeout"]["read"]
        timeouts.append(read)
        clock.now += read  # 応答が来ないままタイムアウトまで待った
        raise httpx.ReadTimeout("timed out", request=req)

    stats = wd.Stats()
    with pytest.raises(httpx.ReadTimeout):
        wd.wait_ready(_client(handler, stats), max_sec=20, stats=stats)
    # 8 秒 → 2 秒待ち → 8 秒 → 2 秒待ち → 残り 0 秒で終了 (合計 20 秒ちょうど)
    assert clock.now - 1000.0 <= 20.0 + 1e-9
    assert timeouts == [8.0, 8.0]
    assert all(t <= 8.0 for t in timeouts)
    assert stats.cold_hello == {
        "ok": False,
        "ms": 20000,
        "attempts": 2,
        "codes": ["ReadTimeout", "ReadTimeout"],
    }


def test_wait_ready_trims_last_timeout_to_remaining(monkeypatch) -> None:
    clock = FakeClock()
    monkeypatch.setattr(wd.time, "monotonic", clock.monotonic)
    monkeypatch.setattr(wd.time, "sleep", clock.sleep)
    timeouts: list[float] = []

    def handler(req):
        read = req.extensions["timeout"]["read"]
        timeouts.append(read)
        clock.now += 1.0  # 1 秒で 503 が返る
        return httpx.Response(503, json={"error": "x"})

    stats = wd.Stats()
    with pytest.raises(wd.EdgeHTTPError):
        wd.wait_ready(_client(handler, stats), max_sec=5, stats=stats)
    # 0s: 5 秒 → 1s で 503、2 秒待ち → 3s: 残り 2 秒 → 4s で 503、1 秒待ち → 5s で終了
    assert timeouts == [5.0, 2.0]
    assert clock.slept == [2.0, 1.0]
    assert clock.now - 1000.0 == 5.0
    assert stats.cold_hello["attempts"] == 2
    assert stats.cold_hello["codes"] == ["503:x", "503:x"]


def test_wait_ready_single_attempt_when_zero() -> None:
    stats = wd.Stats()
    edge = _client(lambda req: httpx.Response(500, json={"error": "boom"}), stats)
    with pytest.raises(wd.EdgeHTTPError):
        wd.wait_ready(edge, max_sec=0, stats=stats)
    assert stats.cold_hello["ok"] is False
    assert stats.cold_hello["attempts"] == 1
    assert stats.cold_hello["codes"] == ["500:boom"]


def _save_flow(photo_reply):
    """auto_session の保存経路だけを通す edge (start / timeout / review / photo / cancel)。"""

    def handler(req):
        path = req.url.path
        if path == "/v1/sessions":
            return httpx.Response(200, json={"ok": True})
        if path.endswith("/timeout"):
            return httpx.Response(200, json={"candidate": {"frame_id": 1, "reason": "best"}})
        if path.endswith("/review"):
            return httpx.Response(202, json={"status": "uploading"})
        if path.endswith("/photo"):
            return httpx.Response(200, json=photo_reply)
        if path.endswith("/cancel"):
            return httpx.Response(200, json={"ok": True})
        return httpx.Response(404, json={"error": "not_found"})

    return handler


def _opt() -> wd.Options:
    return wd.Options(
        fmt="jpeg",
        byte_order="little",
        width=320,
        height=240,
        fps=5,
        compose_sec=0,
        capture_sec=0,
        open_browser=False,
    )


@pytest.mark.parametrize(
    ("photo_reply", "expected"),
    [
        (
            {"status": "ready", "photo_url": "u", "share_url": "s", "expires_at": "e"},
            "saved",
        ),
        ({"status": "error", "reason": "upload_failed"}, "save_failed"),
    ],
)
def test_auto_session_separates_saved_and_failed(monkeypatch, photo_reply, expected) -> None:
    monkeypatch.setattr(wd, "run_phases", lambda *a, **k: False)
    stats = wd.Stats()
    edge = _client(_save_flow(photo_reply), stats)
    outcome = wd.auto_session(edge, None, _opt(), auto_save=True, heartbeat_sec=0)
    assert outcome == expected
    if expected == "saved":
        assert stats.photo_ready_ms and not stats.save_errors
    else:
        assert stats.save_errors == {"upload:upload_failed": 1}


def test_auto_session_save_timeout_is_failure(monkeypatch) -> None:
    clock = FakeClock()
    monkeypatch.setattr(wd.time, "monotonic", clock.monotonic)
    monkeypatch.setattr(wd.time, "sleep", clock.sleep)
    monkeypatch.setattr(wd, "run_phases", lambda *a, **k: False)
    stats = wd.Stats()
    edge = _client(_save_flow({"status": "pending"}), stats)
    outcome = wd.auto_session(edge, None, _opt(), auto_save=True, heartbeat_sec=0)
    assert outcome == "save_failed"
    assert stats.save_errors == {"timeout": 1}
