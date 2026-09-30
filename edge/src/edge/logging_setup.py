"""構造化ログ: 1 行 1 イベントの JSON。

画像バイト列・トークン・URL・鍵は出さない (spec §9, design §9)。
呼び出し側は log_event() を使う。うっかり渡した値も _sanitize() で落とす。
"""

from __future__ import annotations

import json
import logging
import sys
import time
from typing import Any

# 名前にこれらを含むキーは値ごと捨てる (photo_url, share_url, token, device_key など)
_FORBIDDEN_KEY_PARTS = ("url", "token", "key", "secret", "password", "bytes", "data", "jpeg")

_RESERVED = set(vars(logging.makeLogRecord({})).keys()) | {"message", "asctime"}


def _sanitize(fields: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for k, v in fields.items():
        lk = k.lower()
        if any(part in lk for part in _FORBIDDEN_KEY_PARTS):
            continue
        if isinstance(v, bytes | bytearray | memoryview):
            continue
        if hasattr(v, "__array__"):  # numpy 配列 (画像) は出さない
            continue
        if isinstance(v, str) and ("://" in v or len(v) > 300):
            continue  # URL らしき文字列・巨大な文字列は出さない
        if isinstance(v, float):
            v = round(v, 4)
        out[k] = v
    return out


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        payload: dict[str, Any] = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(record.created))
            + f".{int(record.msecs):03d}",
            "level": record.levelname.lower(),
            "event": getattr(record, "event", None) or "log",
        }
        extra = {
            k: v
            for k, v in vars(record).items()
            if k not in _RESERVED and k not in ("event", "fields", "color_message")
        }
        extra.update(getattr(record, "fields", None) or {})
        msg = record.getMessage()
        if payload["event"] == "log":
            payload["logger"] = record.name
        # URL を含むメッセージ (uvicorn の起動行など) は出さない
        if msg != payload["event"] and "://" not in msg:
            payload["message"] = msg
        payload.update(_sanitize(extra))
        if record.exc_info:
            payload["error"] = record.exc_info[0].__name__ if record.exc_info[0] else "error"
        return json.dumps(payload, ensure_ascii=False, default=str)


def setup_logging(level: int = logging.INFO, stream: Any = None) -> None:
    handler = logging.StreamHandler(stream or sys.stderr)
    handler.setFormatter(JsonFormatter())
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
    root.addHandler(handler)
    root.setLevel(level)
    # アクセスログはパスにトークンを含むので出さない (/mock/p/<token>)
    logging.getLogger("uvicorn.access").disabled = True
    for name in ("uvicorn", "uvicorn.error"):
        lg = logging.getLogger(name)
        lg.handlers.clear()
        lg.propagate = True


_log = logging.getLogger("edge")


def log_event(event: str, level: int = logging.INFO, **fields: Any) -> None:
    """event 名と任意のフィールドを 1 行の JSON として出す。"""
    _log.log(level, event, extra={"event": event, "fields": _sanitize(fields)})
