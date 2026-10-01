from __future__ import annotations

import logging
from datetime import UTC, datetime

import httpx
import pytest

from edge.gallery import HttpGallery

BASE_URL = "https://gallery.test"
KEY = "very-secret-gallery-key"
SESSION = "123e4567-e89b-42d3-a456-426614174000"
CAPTURED = datetime(2026, 10, 1, 5, 30, tzinfo=UTC)
JPEG = b"\xff\xd8\xff\xd9"
RESULT = {
    "photo_id": "photo-id",
    "token": "token",
    "photo_url": "https://gallery.test/p/token",
    "share_url": "https://gallery.test/share/x",
    "expires_at": "2026-10-01T15:30:00+09:00",
}


def _gallery(handler) -> HttpGallery:
    return HttpGallery(
        BASE_URL,
        KEY,
        1.0,
        transport=httpx.MockTransport(handler),
        sleep=lambda _seconds: None,
    )


@pytest.mark.parametrize("status", [201, 200])
def test_upload_success_and_idempotent_status(status: int) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url == f"{BASE_URL}/internal/photos"
        assert request.headers["X-Gallery-Key"] == KEY
        assert request.headers["X-Session-Id"] == SESSION
        assert request.headers["X-Captured-At"] == CAPTURED.isoformat()
        assert request.headers["Content-Type"] == "image/jpeg"
        assert request.content == JPEG
        return httpx.Response(status, json=RESULT)

    result = _gallery(handler).upload(JPEG, SESSION, CAPTURED)
    assert result.photo_id == "photo-id"
    assert result.photo_url == RESULT["photo_url"]
    assert result.share_url == RESULT["share_url"]
    assert result.expires_at == datetime(2026, 10, 1, 15, 30, tzinfo=RESULT_TZ)


RESULT_TZ = datetime.fromisoformat(RESULT["expires_at"]).tzinfo


def test_upload_401_is_not_retried() -> None:
    calls = 0

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        return httpx.Response(401, json={"error": "unauthorized"})

    with pytest.raises(httpx.HTTPStatusError):
        _gallery(handler).upload(JPEG, SESSION, CAPTURED)
    assert calls == 1


def test_upload_retries_5xx_then_succeeds() -> None:
    calls = 0

    def handler(_request: httpx.Request) -> httpx.Response:
        nonlocal calls
        calls += 1
        if calls < 3:
            return httpx.Response(503)
        return httpx.Response(201, json=RESULT)

    assert _gallery(handler).upload(JPEG, SESSION, CAPTURED).photo_id == "photo-id"
    assert calls == 3


def test_key_never_appears_in_logs(caplog: pytest.LogCaptureFixture) -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        if request.headers["X-Gallery-Key"] == KEY:
            return httpx.Response(503)
        raise AssertionError("key header missing")

    caplog.set_level(logging.DEBUG)
    with pytest.raises(httpx.HTTPStatusError):
        _gallery(handler).upload(JPEG, SESSION, CAPTURED)
    _gallery(handler).delete("full-photo-token")
    assert KEY not in caplog.text
    assert "full-photo-token" not in caplog.text
