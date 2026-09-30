from __future__ import annotations

import base64
from datetime import UTC, datetime
from urllib.parse import parse_qs, urlparse

import numpy as np
from conftest import FakeClock
from fastapi.testclient import TestClient

from edge.gallery import MockGallery
from edge.image import encode_jpeg

JPEG = encode_jpeg(np.full((8, 8, 3), 128, dtype=np.uint8))
CAPTURED = datetime(2026, 9, 30, 12, 0, tzinfo=UTC)


def _token(url: str) -> str:
    return url.rsplit("/", 1)[1]


def test_token_is_128_bit(gallery: MockGallery) -> None:
    tokens = {_token(gallery.upload(JPEG, f"s{i}", CAPTURED).photo_url) for i in range(20)}
    assert len(tokens) == 20
    for t in tokens:
        raw = base64.urlsafe_b64decode(t + "=" * (-len(t) % 4))
        assert len(raw) == 16 and len(t) == 22


def test_upload_idempotent_per_session_and_image(gallery: MockGallery) -> None:
    a = gallery.upload(JPEG, "s1", CAPTURED)
    assert gallery.upload(JPEG, "s1", CAPTURED) == a
    other = encode_jpeg(np.zeros((8, 8, 3), dtype=np.uint8))
    assert gallery.upload(other, "s1", CAPTURED).photo_url != a.photo_url  # retake 後の別画像


def test_ttl_then_410(client: TestClient, gallery: MockGallery, clock: FakeClock) -> None:
    res = gallery.upload(JPEG, "s1", CAPTURED)
    path = urlparse(res.photo_url).path
    page = client.get(path)
    assert page.status_code == 200
    assert "URL を知る人は誰でも見られます" in page.text
    assert "削除予定時刻: 2026-09-30 22:00" in page.text
    assert "モック" in page.text
    assert page.headers["cache-control"] == "no-store"
    assert page.headers["referrer-policy"] == "no-referrer"
    assert "noindex" in page.headers["x-robots-tag"]
    assert client.get(path + ".jpg").content == JPEG

    clock.advance(minutes=59, seconds=59)
    assert client.get(path).status_code == 200
    clock.advance(seconds=1)
    assert client.get(path).status_code == 410
    assert client.get(path + ".jpg").status_code == 410
    gallery.sweep()
    assert client.get(path + ".jpg").status_code == 410


def test_unknown_token_404(client: TestClient) -> None:
    assert client.get("/mock/p/nope").status_code == 404
    assert client.get("/mock/p/nope.jpg").status_code == 404


def test_share_redirect_has_text_only(client: TestClient, gallery: MockGallery) -> None:
    res = gallery.upload(JPEG, "s1", CAPTURED)
    r = client.get("/mock/share/x", follow_redirects=False)
    assert r.status_code == 302
    loc = r.headers["location"]
    assert loc.startswith("https://x.com/intent/tweet?text=")
    text = parse_qs(urlparse(loc).query)["text"][0]
    assert text == "スタックチャンに撮ってもらいました！ #スタックチャン #StackChan"
    assert _token(res.photo_url) not in loc
    assert "edge.test" not in loc and "/mock/p/" not in loc
