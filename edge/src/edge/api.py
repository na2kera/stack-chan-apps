"""HTTP ルーティング (docs/protocol.md)。認証と JSON 変換だけを行い、処理は session.py に任せる。"""

from __future__ import annotations

import threading
from collections.abc import AsyncIterator, Callable
from contextlib import asynccontextmanager
from typing import Annotated, Any, Literal

from fastapi import APIRouter, Depends, FastAPI, Header, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.exceptions import RequestValidationError
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response
from pydantic import BaseModel

from edge.auth import verify_device
from edge.config import AuthConfig
from edge.gallery import (
    MockGallery,
    PhotoExpired,
    render_expired_page,
    render_not_found_page,
    render_photo_page,
)
from edge.logging_setup import log_event
from edge.session import FrameInput, PhotoboothService, ServiceError

MAX_FRAME_BYTES = 2 * 1024 * 1024
SWEEP_INTERVAL_SEC = 30.0

# 写真ページ用 (spec §7.1: キャッシュ無効、no-referrer、インデックス対象外)
PRIVATE_HEADERS = {
    "Cache-Control": "no-store",
    "Referrer-Policy": "no-referrer",
    "X-Robots-Tag": "noindex, nofollow",
}


class HelloBody(BaseModel):
    device_id: str
    protocol_version: int


class SessionStartBody(BaseModel):
    session_id: str
    started_at_ms: int = 0


class ReviewBody(BaseModel):
    decision: Literal["save", "retake"]


def _int_header(request: Request, name: str) -> int:
    value = request.headers.get(name)
    if value is None:
        raise ServiceError(400, f"missing_header:{name}")
    try:
        return int(value)
    except ValueError as exc:
        raise ServiceError(400, f"invalid_header:{name}") from exc


def _choice_header(request: Request, name: str, choices: tuple[str, ...]) -> str:
    value = (request.headers.get(name) or "").lower()
    if value not in choices:
        raise ServiceError(400, f"invalid_header:{name}")
    return value


def create_app(
    service: PhotoboothService,
    auth: AuthConfig,
    gallery: MockGallery | None = None,
    background_sweep: bool = True,
) -> FastAPI:
    def require_device(
        x_device_id: Annotated[str | None, Header()] = None,
        x_device_key: Annotated[str | None, Header()] = None,
    ) -> None:
        if not verify_device(x_device_id, x_device_key, auth):
            raise ServiceError(401, "unauthorized")

    @asynccontextmanager
    async def lifespan(_app: FastAPI) -> AsyncIterator[None]:
        stop = threading.Event()
        thread = None
        if background_sweep:
            thread = threading.Thread(
                target=_sweep_loop, args=(stop, service, gallery), daemon=True, name="sweeper"
            )
            thread.start()
        try:
            yield
        finally:
            stop.set()
            if thread:
                thread.join(timeout=2)

    app = FastAPI(
        title="stackchan-photobooth-edge", lifespan=lifespan, docs_url=None, redoc_url=None
    )

    @app.exception_handler(ServiceError)
    async def _service_error(_req: Request, exc: ServiceError) -> JSONResponse:
        return JSONResponse(status_code=exc.status, content={"error": exc.code})

    @app.exception_handler(RequestValidationError)
    async def _validation_error(_req: Request, _exc: RequestValidationError) -> JSONResponse:
        return JSONResponse(status_code=400, content={"error": "bad_request"})

    v1 = APIRouter(prefix="/v1", dependencies=[Depends(require_device)])

    def call(fn: Callable[..., Any], *args: Any) -> Any:
        service.sweep()
        return fn(*args)

    @v1.post("/hello")
    def hello(body: HelloBody) -> dict[str, Any]:
        return call(service.hello, body.device_id, body.protocol_version)

    @v1.post("/sessions")
    def session_start(body: SessionStartBody) -> dict[str, Any]:
        return call(service.start_session, body.session_id, body.started_at_ms)

    @v1.post("/sessions/{session_id}/frames")
    async def frame(session_id: str, request: Request) -> dict[str, Any]:
        length = request.headers.get("content-length")
        if length is not None and length.isdigit() and int(length) > MAX_FRAME_BYTES:
            raise ServiceError(413, "frame_too_large")
        data = await request.body()
        if len(data) > MAX_FRAME_BYTES:
            raise ServiceError(413, "frame_too_large")
        if not data:
            raise ServiceError(400, "empty_frame")
        f = FrameInput(
            session_id=session_id,
            frame_id=_int_header(request, "x-frame-id"),
            capture_ms=_int_header(request, "x-capture-ms"),
            servo_x=_int_header(request, "x-servo-x"),
            servo_y=_int_header(request, "x-servo-y"),
            width=_int_header(request, "x-width"),
            height=_int_header(request, "x-height"),
            fmt=_choice_header(request, "x-format", ("rgb565", "jpeg")),  # type: ignore[arg-type]
            phase=_choice_header(request, "x-phase", ("compose", "capture")),  # type: ignore[arg-type]
            data=data,
        )
        # 解析は重い同期処理なのでイベントループを塞がないようスレッドで回す
        return await run_in_threadpool(call, service.process_frame, f)

    @v1.post("/sessions/{session_id}/timeout")
    def timeout(session_id: str) -> dict[str, Any]:
        return call(service.timeout, session_id)

    @v1.post("/sessions/{session_id}/review")
    def review(session_id: str, body: ReviewBody) -> JSONResponse:
        status, content = call(service.review, session_id, body.decision)
        return JSONResponse(status_code=status, content=content)

    @v1.get("/sessions/{session_id}/photo")
    def photo(session_id: str) -> dict[str, Any]:
        return call(service.photo, session_id)

    @v1.post("/sessions/{session_id}/cancel")
    def cancel(session_id: str) -> dict[str, Any]:
        return call(service.cancel, session_id)

    app.include_router(v1)

    if gallery is not None:
        _add_mock_routes(app, gallery)
    return app


def _add_mock_routes(app: FastAPI, gallery: MockGallery) -> None:
    """モック gallery の閲覧用ルート。

    認証なし (スマホ・ブラウザから開く)。トークンはログに出さない。
    """

    @app.get("/mock/p/{token}.jpg")
    def mock_photo_jpeg(token: str) -> Response:
        try:
            photo = gallery.get(token)
        except PhotoExpired:
            log_event("mock_view", reason="expired")
            return Response(status_code=410, headers=PRIVATE_HEADERS)
        if photo is None or photo.jpeg is None:
            return Response(status_code=404, headers=PRIVATE_HEADERS)
        log_event("mock_view", reason="jpeg")
        return Response(
            content=photo.jpeg,
            media_type="image/jpeg",
            headers={**PRIVATE_HEADERS, "Content-Disposition": 'inline; filename="stackchan.jpg"'},
        )

    @app.get("/mock/p/{token}")
    def mock_photo_page(token: str) -> HTMLResponse:
        try:
            photo = gallery.get(token)
        except PhotoExpired:
            log_event("mock_view", reason="expired")
            return HTMLResponse(render_expired_page(), status_code=410, headers=PRIVATE_HEADERS)
        if photo is None:
            return HTMLResponse(render_not_found_page(), status_code=404, headers=PRIVATE_HEADERS)
        log_event("mock_view", reason="page")
        return HTMLResponse(render_photo_page(photo), headers=PRIVATE_HEADERS)

    @app.get("/mock/share/x")
    def mock_share_x() -> RedirectResponse:
        return RedirectResponse(
            gallery.share_redirect_url(), status_code=302, headers=PRIVATE_HEADERS
        )


def _sweep_loop(
    stop: threading.Event, service: PhotoboothService, gallery: MockGallery | None
) -> None:
    while not stop.wait(SWEEP_INTERVAL_SEC):
        try:
            service.sweep()
            if gallery is not None:
                gallery.sweep()
        except Exception as exc:  # noqa: BLE001 - 掃除の失敗でサーバーを止めない
            log_event("sweep_failed", reason=type(exc).__name__)
