"""X-Device-Id / X-Device-Key の照合 (protocol.md「認証」)。"""

from __future__ import annotations

import hmac

from edge.config import AuthConfig


def verify_device(device_id: str | None, device_key: str | None, cfg: AuthConfig) -> bool:
    """両方が一致すれば True。比較は定数時間 (hmac.compare_digest)。"""
    if not device_id or not device_key:
        return False
    id_ok = hmac.compare_digest(device_id.encode(), cfg.device_id.encode())
    key_ok = hmac.compare_digest(device_key.encode(), cfg.device_key.encode())
    return id_ok and key_ok
