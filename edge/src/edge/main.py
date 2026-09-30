"""`uv run edge` の入口。設定を読み、モデルをロードして uvicorn を起動する。"""

from __future__ import annotations

import argparse
import logging
import socket
import sys
from datetime import timedelta
from pathlib import Path

from edge.config import Config, ConfigError, load_config
from edge.logging_setup import log_event, setup_logging


def lan_address() -> str:
    """0.0.0.0 で listen するとき、モックの写真 URL に入れる PC の LAN アドレスを推定する。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # 実際には送信しない
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def mock_base_url(cfg: Config) -> str:
    if cfg.gallery.public_base_url:
        return cfg.gallery.public_base_url
    host = cfg.server.host
    if host in ("0.0.0.0", "", "::"):
        host = lan_address()
    return f"http://{host}:{cfg.server.port}"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="edge", description="StackChan photobooth edge")
    parser.add_argument("--config", type=Path, default=None, help="config.toml のパス")
    parser.add_argument("--host", default=None, help="[server] host を上書き")
    parser.add_argument("--port", type=int, default=None, help="[server] port を上書き")
    parser.add_argument("--debug", action="store_true", help="DEBUG ログを出す")
    args = parser.parse_args(argv)

    setup_logging(logging.DEBUG if args.debug else logging.INFO)
    try:
        cfg = load_config(args.config)
    except (OSError, ConfigError, ValueError) as exc:
        log_event("config_error", logging.ERROR, reason=str(exc))
        return 2
    if args.host or args.port:
        from dataclasses import replace

        server = replace(
            cfg.server, host=args.host or cfg.server.host, port=args.port or cfg.server.port
        )
        cfg = replace(cfg, server=server)

    # 重い import はここで行う (テストでは読み込まない)
    import uvicorn

    from edge.analysis import MediaPipeAnalyzer, ModelNotFoundError
    from edge.api import create_app
    from edge.gallery import MockGallery
    from edge.session import PhotoboothService

    try:
        analyzer = MediaPipeAnalyzer(cfg.model_file(), cfg.capture.max_faces)
    except ModelNotFoundError as exc:
        log_event("model_missing", logging.ERROR, reason=str(exc))
        print(str(exc), file=sys.stderr)
        return 2
    log_event("model_loaded", max_faces=cfg.capture.max_faces)

    gallery = MockGallery(
        base_url=mock_base_url(cfg),
        ttl=timedelta(minutes=cfg.gallery.ttl_minutes),
        share_text=cfg.share.text,
    )
    service = PhotoboothService(cfg, analyzer, gallery)
    app = create_app(service, cfg.auth, gallery)
    log_event(
        "server_start",
        host=cfg.server.host,
        port=cfg.server.port,
        gallery=cfg.gallery.mode,
        ttl_minutes=cfg.gallery.ttl_minutes,
        rgb565_byte_order=cfg.capture.rgb565_byte_order,
    )
    uvicorn.run(app, host=cfg.server.host, port=cfg.server.port, access_log=False, log_config=None)
    return 0


if __name__ == "__main__":
    sys.exit(main())
