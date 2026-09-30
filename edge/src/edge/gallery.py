"""写真の配布先。ステップ2a では MockGallery (メモリ + TTL) だけ。

MockGallery は edge 自身が /mock/p/<token> でページを配る。LAN 内の動作確認用で、
スマホに QR で配る用途には使わない (spec 仮定 B: PC の LAN アドレスを QR に入れない)。
"""

from __future__ import annotations

import hashlib
import html
import secrets
import threading
from collections.abc import Callable
from dataclasses import dataclass
from datetime import UTC, datetime, timedelta, timezone
from typing import Protocol
from urllib.parse import quote

JST = timezone(timedelta(hours=9), "JST")
X_INTENT = "https://x.com/intent/tweet?text="


@dataclass(frozen=True)
class UploadResult:
    photo_id: str  # delete() に渡す識別子 (mock ではトークン。ログには出さない)
    photo_url: str
    share_url: str
    expires_at: datetime  # UTC


class Gallery(Protocol):
    def upload(self, jpeg: bytes, session_id: str, captured_at: datetime) -> UploadResult: ...

    def delete(self, photo_id: str) -> None:
        """写真を直ちに消す (画像もページも配信しない)。未知の ID は無視する。"""
        ...


@dataclass
class MockPhoto:
    token: str
    session_id: str
    digest: str
    jpeg: bytes | None
    captured_at: datetime
    expires_at: datetime


class PhotoExpired(Exception):
    pass


class MockGallery:
    def __init__(
        self,
        base_url: str,
        ttl: timedelta,
        share_text: str,
        now: Callable[[], datetime] | None = None,
    ) -> None:
        self._base = base_url.rstrip("/")
        self._ttl = ttl
        self.share_text = share_text
        self._now = now or (lambda: datetime.now(UTC))
        self._photos: dict[str, MockPhoto] = {}
        self._by_session: dict[tuple[str, str], str] = {}
        self._lock = threading.Lock()

    # ---- edge から ----

    def upload(self, jpeg: bytes, session_id: str, captured_at: datetime) -> UploadResult:
        """同じ session_id・同じ画像の再送は同じ写真を返す (冪等、spec §7.1)。

        retake 後に同じ session_id で別の画像が来たら別の写真として扱う。
        前の写真が期限切れなら、同じ画像でも新しいトークンで登録し直す。
        """
        if not jpeg.startswith(b"\xff\xd8"):
            raise ValueError("not a JPEG")
        digest = hashlib.sha256(jpeg).hexdigest()
        with self._lock:
            self._sweep_locked()
            token = self._by_session.get((session_id, digest))
            photo = self._photos.get(token) if token else None
            if photo is not None and (photo.jpeg is None or self._now() >= photo.expires_at):
                photo = None  # 期限切れの URL は返さず、新しい写真として登録し直す
            if photo is None:
                token = secrets.token_urlsafe(16)  # 128 ビット
                photo = MockPhoto(
                    token=token,
                    session_id=session_id,
                    digest=digest,
                    jpeg=jpeg,
                    captured_at=captured_at,
                    expires_at=self._now() + self._ttl,
                )
                self._photos[token] = photo
                self._by_session[(session_id, digest)] = token
        return UploadResult(
            photo_id=photo.token,
            photo_url=f"{self._base}/mock/p/{photo.token}",
            share_url=f"{self._base}/mock/share/x",
            expires_at=photo.expires_at,
        )

    def delete(self, photo_id: str) -> None:
        """画像とメタデータを直ちに消す。以後その URL は 404。"""
        with self._lock:
            photo = self._photos.pop(photo_id, None)
            if photo is not None:
                photo.jpeg = None
                if self._by_session.get((photo.session_id, photo.digest)) == photo_id:
                    del self._by_session[(photo.session_id, photo.digest)]

    # ---- 閲覧 (スマホ / ブラウザ) から ----

    def get(self, token: str) -> MockPhoto | None:
        """未知なら None。期限切れなら PhotoExpired (410 にする)。"""
        with self._lock:
            photo = self._photos.get(token)
            if photo is None:
                return None
            if self._now() >= photo.expires_at:
                photo.jpeg = None  # 削除ジョブ前でもアプリ層で配信しない
                raise PhotoExpired()
            return photo

    def share_redirect_url(self) -> str:
        # 本文は設定の文言とタグだけ。写真 URL やトークンは入れない (spec §7.2)
        return X_INTENT + quote(self.share_text, safe="")

    def sweep(self) -> int:
        with self._lock:
            return self._sweep_locked()

    def _sweep_locked(self) -> int:
        """期限切れの画像バイト列を消す。token は 410 を返すため期限後 1 日だけ残す。"""
        now = self._now()
        removed = 0
        for token, p in list(self._photos.items()):
            if now >= p.expires_at and p.jpeg is not None:
                p.jpeg = None
                removed += 1
            if now >= p.expires_at + timedelta(days=1):
                del self._photos[token]
                self._by_session.pop((p.session_id, p.digest), None)
        return removed


# ---- ページ ----

_STYLE = """
body{font-family:system-ui,-apple-system,"Hiragino Sans",sans-serif;margin:0;padding:16px;
background:#fafafa;color:#222;max-width:640px;margin:auto}
img{width:100%;height:auto;border-radius:8px;background:#ddd}
.btn{display:block;text-align:center;padding:14px;margin:16px 0;border-radius:8px;
background:#1d9bf0;color:#fff;text-decoration:none;font-weight:bold}
.note{font-size:.9em;color:#555}.warn{font-size:.9em;color:#a33}
"""


def _page(title: str, body: str) -> str:
    return (
        '<!doctype html><html lang="ja"><head><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1">'
        '<meta name="robots" content="noindex,nofollow">'
        '<meta name="referrer" content="no-referrer">'
        f"<title>{html.escape(title)}</title><style>{_STYLE}</style></head>"
        f"<body>{body}</body></html>"
    )


def format_jst(dt: datetime) -> str:
    return dt.astimezone(JST).strftime("%Y-%m-%d %H:%M")


def render_photo_page(photo: MockPhoto) -> str:
    img = f"/mock/p/{html.escape(photo.token)}.jpg"
    body = (
        "<h1>スタックチャンの写真</h1>"
        f'<img src="{img}" alt="写真">'
        f'<a class="btn" href="{img}" download="stackchan.jpg">写真をダウンロード</a>'
        f"<p>削除予定時刻: {format_jst(photo.expires_at)}（日本時間）</p>"
        "<p class=note>URL を知る人は誰でも見られます。</p>"
        "<p class=warn>これは LAN 内の動作確認用のモックです。"
        "QR でスマホに配る用途には使わないでください。</p>"
    )
    return _page("スタックチャンの写真", body)


def render_expired_page() -> str:
    return _page(
        "写真の期限切れ", "<h1>この写真は削除されました</h1><p>保存期限を過ぎています。</p>"
    )


def render_not_found_page() -> str:
    return _page("写真が見つかりません", "<h1>写真が見つかりません</h1>")
