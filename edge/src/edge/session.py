"""撮影セッション (design §7)。

Session は 1 回の撮影の状態を持ち、SessionStore は dict + lock で複数セッションを管理する。
PhotoboothService は api.py から呼ばれる入口で、解析・判定・首振り・gallery を組み合わせる。

スレッド: FastAPI は同期処理をスレッドプールで並行に呼ぶ。
- SessionStore._lock: dict の出し入れと touched の更新を守る。
- Session.lock: 1 セッションのフレームを到着順に 1 枚ずつ処理し、状態の読み書きを守る。
  (両方取るときは session → store の順。store を持ったまま session.lock を取らない)
- MediaPipeAnalyzer は内部ロックで直列化される。
"""

from __future__ import annotations

import logging
import threading
import time
import uuid
from collections.abc import Callable
from concurrent.futures import Executor, ThreadPoolExecutor
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from typing import Any, Literal

import numpy as np

from edge.analysis import FaceAnalyzer
from edge.config import Config
from edge.decision import (
    CandidateScore,
    CountState,
    accept_step,
    candidate_score,
    evaluate_frame,
    is_better,
)
from edge.gallery import JST, Gallery
from edge.head import HeadState, compute_head
from edge.image import ImageDecodeError, decode_frame, encode_jpeg, sharpness
from edge.logging_setup import log_event

PROTOCOL_VERSION = 1
SESSION_IDLE_SEC = 300.0  # 5 分イベントの無いセッションを破棄 (protocol.md)
JPEG_QUALITY = 90


class State(StrEnum):
    COMPOSE = "compose"
    CAPTURE = "capture"
    TIMEOUT = "timeout"  # 時間切れで候補なし
    REVIEW = "review"  # 採用済み、または時間切れで候補あり。save / retake 待ち
    UPLOADING = "uploading"
    DONE = "done"
    CANCELLED = "cancelled"


class ServiceError(Exception):
    def __init__(self, status: int, code: str) -> None:
        super().__init__(code)
        self.status = status
        self.code = code


@dataclass(frozen=True)
class FrameInput:
    session_id: str
    frame_id: int
    capture_ms: int
    servo_x: int
    servo_y: int
    width: int
    height: int
    fmt: Literal["rgb565", "jpeg"]
    phase: Literal["compose", "capture"]
    data: bytes


@dataclass
class RetainedFrame:
    """採用フレーム / 候補フレーム。判定に使ったバイト列そのもの (spec §4)。"""

    frame_id: int
    fmt: str
    raw: bytes  # 受信したままの RGB565 / JPEG
    rgb: np.ndarray  # 判定に使った RGB
    score: CandidateScore
    received_at: datetime


@dataclass(frozen=True)
class PhotoInfo:
    photo_url: str
    share_url: str
    expires_at: datetime


class Session:
    def __init__(self, session_id: str, now: float) -> None:
        self.session_id = session_id
        self.lock = threading.Lock()
        self.touched = now
        self.generation = 0
        self.reset()

    def reset(self) -> None:
        """状態を初期化し、保持バイト列を捨てる (session_start の再送・retake)。"""
        self.generation += 1  # 進行中のアップロード結果を無効にする
        self.state = State.COMPOSE
        self.count = CountState()
        self.head = HeadState()
        self.last_frame_id = -1
        self.consecutive_met = 0
        self.accepted: RetainedFrame | None = None
        self.best: RetainedFrame | None = None
        self.photo: PhotoInfo | None = None
        self.photo_error: str | None = None

    def discard_frames(self) -> None:
        self.accepted = None
        self.best = None

    def chosen(self) -> RetainedFrame | None:
        return self.accepted or self.best


class SessionStore:
    def __init__(
        self, idle_sec: float = SESSION_IDLE_SEC, clock: Callable[[], float] = time.monotonic
    ) -> None:
        self._sessions: dict[str, Session] = {}
        self._lock = threading.Lock()
        self._idle_sec = idle_sec
        self._clock = clock

    def start(self, session_id: str) -> Session:
        with self._lock:
            now = self._clock()
            session = self._sessions.get(session_id)
            if session is None:
                session = Session(session_id, now)
                self._sessions[session_id] = session
                return session
            session.touched = now
        with session.lock:
            session.reset()
        return session

    def get(self, session_id: str) -> Session:
        # touched の更新も store のロックの中で行い、sweep() との間に隙間を作らない
        with self._lock:
            session = self._sessions.get(session_id)
            if session is None:
                raise ServiceError(404, "unknown_session")
            session.touched = self._clock()
        return session

    def sweep(self) -> list[str]:
        """idle_sec 以上イベントの無いセッションを消す。

        候補を選んだあと、セッションのロック → store のロックの順に取り直して
        touched を再確認する (その間に get() / start() があれば消さない)。
        """
        with self._lock:
            now = self._clock()
            candidates = [s for s in self._sessions.values() if now - s.touched >= self._idle_sec]
        removed: list[str] = []
        for s in candidates:
            with s.lock:
                with self._lock:
                    if self._sessions.get(s.session_id) is not s:
                        continue
                    if self._clock() - s.touched < self._idle_sec:
                        continue
                    del self._sessions[s.session_id]
                s.discard_frames()
                s.generation += 1
                removed.append(s.session_id)
        return removed

    def states(self) -> list[State]:
        with self._lock:
            return [s.state for s in self._sessions.values()]

    def __len__(self) -> int:
        with self._lock:
            return len(self._sessions)


def _iso_jst(dt: datetime) -> str:
    return dt.astimezone(JST).isoformat(timespec="seconds")


class PhotoboothService:
    def __init__(
        self,
        cfg: Config,
        analyzer: FaceAnalyzer,
        gallery: Gallery,
        store: SessionStore | None = None,
        executor: Executor | None = None,
        wall_clock: Callable[[], datetime] | None = None,
    ) -> None:
        self.cfg = cfg
        self.analyzer = analyzer
        self.gallery = gallery
        self.store = store if store is not None else SessionStore()
        self._executor = (
            executor
            if executor is not None
            else ThreadPoolExecutor(max_workers=2, thread_name_prefix="upload")
        )
        self._wall = wall_clock or (lambda: datetime.now(UTC))

    # ---- hello / session_start ----

    def hello(self, device_id: str, protocol_version: int) -> dict[str, Any]:
        if protocol_version != PROTOCOL_VERSION:
            raise ServiceError(400, "unsupported_protocol_version")
        busy = any(
            s in (State.COMPOSE, State.CAPTURE, State.UPLOADING) for s in self.store.states()
        )
        log_event("hello", device_id=device_id, protocol_version=protocol_version)
        return {
            "ready": True,
            "edge_state": "busy" if busy else "idle",
            "max_faces": self.cfg.capture.max_faces,
            "countdown_sec": self.cfg.capture.countdown_sec,
        }

    def start_session(self, session_id: str, started_at_ms: int) -> dict[str, Any]:
        try:
            uuid.UUID(session_id)
        except ValueError as exc:
            raise ServiceError(400, "invalid_session_id") from exc
        self.store.start(session_id)
        log_event("session_start", session_id=session_id, state=State.COMPOSE.value)
        return {"ok": True}

    # ---- frame ----

    def process_frame(self, f: FrameInput) -> dict[str, Any]:
        t0 = time.perf_counter()
        session = self.store.get(f.session_id)
        with session.lock:
            if session.state not in (State.COMPOSE, State.CAPTURE):
                return self._dropped(session, f, t0, "state")
            if f.frame_id <= session.last_frame_id:
                return self._dropped(session, f, t0, "stale")
            session.last_frame_id = f.frame_id

            try:
                rgb = decode_frame(
                    f.fmt, f.data, f.width, f.height, self.cfg.capture.rgb565_byte_order
                )
            except ImageDecodeError as exc:
                log_event(
                    "frame_error",
                    logging.WARNING,
                    session_id=f.session_id,
                    frame_id=f.frame_id,
                    reason=str(exc),
                )
                raise ServiceError(400, "bad_image") from exc

            faces = self.analyzer.analyze(rgb, int(time.monotonic() * 1000))
            height, width = rgb.shape[:2]
            cmd, session.head = compute_head(
                faces,
                width,
                height,
                f.servo_x,
                f.servo_y,
                f.phase,
                f.capture_ms,
                session.head,
                self.cfg.head,
                self.cfg.capture.margin_ratio,
            )
            verdict, session.count = evaluate_frame(
                faces, session.count, self.cfg.capture, head_closer=cmd.closer
            )

            accepted = False
            if f.phase == "capture":
                if session.state == State.COMPOSE:
                    session.state = State.CAPTURE
                    log_event(
                        "state",
                        session_id=f.session_id,
                        state=State.CAPTURE.value,
                        target=session.count.target,
                    )
                session.consecutive_met, accepted = accept_step(
                    session.consecutive_met, verdict.conditions_met, self.cfg.capture
                )
                score = candidate_score(faces, sharpness(rgb) if faces else 0.0, self.cfg.capture)
                if accepted and score is not None:
                    session.accepted = self._retain(f, rgb, score)
                    session.best = None  # 採用フレームがあれば候補は要らない
                    session.state = State.REVIEW
                elif is_better(score, session.best.score if session.best else None):
                    assert score is not None
                    session.best = self._retain(f, rgb, score)  # 最良の 1 枚だけ持つ
            else:
                session.consecutive_met = 0  # COMPOSE 中の達成は採用に数えない

            latency_ms = int((time.perf_counter() - t0) * 1000)
            log_event(
                "frame",
                session_id=f.session_id,
                frame_id=f.frame_id,
                phase=f.phase,
                state=session.state.value,
                face_count=verdict.face_count,
                target=verdict.target_face_count,
                in_frame=verdict.all_in_frame,
                eyes_open=verdict.all_eyes_open,
                smiling=verdict.all_smiling,
                met=verdict.conditions_met,
                servo_x=f.servo_x,
                servo_y=f.servo_y,
                servo_dx=cmd.dx,
                servo_dy=cmd.dy,
                hint=verdict.hint,
                accepted=accepted,
                latency_ms=latency_ms,
            )
            return {
                "session_id": f.session_id,
                "frame_id": f.frame_id,
                "dropped": False,
                "face_count": verdict.face_count,
                "target_face_count": verdict.target_face_count,
                "all_in_frame": verdict.all_in_frame,
                "all_eyes_open": verdict.all_eyes_open,
                "all_smiling": verdict.all_smiling,
                "servo_dx": cmd.dx,
                "servo_dy": cmd.dy,
                "hint": verdict.hint,
                "accepted": accepted,
                "latency_ms": latency_ms,
            }

    def _retain(self, f: FrameInput, rgb: np.ndarray, score: CandidateScore) -> RetainedFrame:
        return RetainedFrame(
            frame_id=f.frame_id,
            fmt=f.fmt,
            raw=f.data,
            rgb=rgb,
            score=score,
            received_at=self._wall(),
        )

    def _dropped(self, session: Session, f: FrameInput, t0: float, why: str) -> dict[str, Any]:
        latency_ms = int((time.perf_counter() - t0) * 1000)
        log_event(
            "frame_dropped",
            session_id=f.session_id,
            frame_id=f.frame_id,
            state=session.state.value,
            reason=why,
            latency_ms=latency_ms,
        )
        return {
            "session_id": f.session_id,
            "frame_id": f.frame_id,
            "dropped": True,
            "face_count": 0,
            "target_face_count": session.count.target,
            "all_in_frame": False,
            "all_eyes_open": False,
            "all_smiling": False,
            "servo_dx": 0,
            "servo_dy": 0,
            "hint": None,
            "accepted": False,
            "latency_ms": latency_ms,
        }

    # ---- timeout ----

    def timeout(self, session_id: str) -> dict[str, Any]:
        session = self.store.get(session_id)
        with session.lock:
            if session.state == State.CANCELLED:
                raise ServiceError(409, "cancelled")
            if session.state in (State.COMPOSE, State.CAPTURE):
                session.state = State.REVIEW if session.chosen() else State.TIMEOUT
            chosen = session.chosen()
            if chosen is None:
                log_event(
                    "timeout", session_id=session_id, state=session.state.value, reason="no_face"
                )
                return {"candidate": None}
            prefix = "accepted " if chosen is session.accepted else ""
            reason = prefix + chosen.score.reason()
            log_event(
                "timeout",
                session_id=session_id,
                frame_id=chosen.frame_id,
                state=session.state.value,
                score=chosen.score.scalar(),
                reason=reason,
            )
            return {
                "candidate": {
                    "frame_id": chosen.frame_id,
                    "score": chosen.score.scalar(),
                    "reason": reason,
                }
            }

    # ---- review ----

    def review(self, session_id: str, decision: str) -> tuple[int, dict[str, Any]]:
        session = self.store.get(session_id)
        if decision == "retake":
            with session.lock:
                # 撮り直しは REVIEW (候補あり・採用済み) と TIMEOUT (候補なし) のときだけ。
                # 公開後 (DONE) や撮影中に reset すると写真や進行中の撮影を壊すので断る
                if session.state not in (State.REVIEW, State.TIMEOUT):
                    raise ServiceError(409, "nothing_to_retake")
                session.reset()  # 保持フレームを破棄して新しい 10 秒へ
            log_event("review", session_id=session_id, reason="retake", state=State.COMPOSE.value)
            return 200, {"ok": True}
        if decision != "save":
            raise ServiceError(400, "invalid_decision")

        with session.lock:
            if session.state in (State.UPLOADING, State.DONE):
                return 202, {"status": "uploading"}  # 再送は冪等
            chosen = session.chosen()
            if session.state != State.REVIEW or chosen is None:
                raise ServiceError(409, "nothing_to_save")
            session.state = State.UPLOADING
            session.photo_error = None
            generation = session.generation
        log_event(
            "review",
            session_id=session_id,
            frame_id=chosen.frame_id,
            reason="save",
            state=State.UPLOADING.value,
        )
        self._executor.submit(self._upload, session, generation, chosen)
        return 202, {"status": "uploading"}

    def _upload(self, session: Session, generation: int, frame: RetainedFrame) -> None:
        t0 = time.perf_counter()
        try:
            jpeg = encode_jpeg(frame.rgb, JPEG_QUALITY)
            result = self.gallery.upload(jpeg, session.session_id, frame.received_at)
        except Exception as exc:  # noqa: BLE001 - 失敗は状態に写して device に返す
            with session.lock:
                if session.generation == generation and session.state == State.UPLOADING:
                    session.state = State.REVIEW  # 保持したまま save の再試行を待つ
                    session.photo_error = "upload_failed"
            log_event(
                "upload_failed",
                logging.ERROR,
                session_id=session.session_id,
                frame_id=frame.frame_id,
                reason=type(exc).__name__,
            )
            return
        with session.lock:
            stale = session.generation != generation or session.state != State.UPLOADING
            if not stale:
                session.photo = PhotoInfo(result.photo_url, result.share_url, result.expires_at)
                session.state = State.DONE
                session.discard_frames()  # 公開後は edge にバイト列を残さない
        if stale:
            # アップロード中に cancel / 再 start / 期限切れ掃除があった: 公開した写真を消す
            deleted = True
            try:
                self.gallery.delete(result.photo_id)
            except Exception as exc:  # noqa: BLE001
                deleted = False
                log_event("gallery_delete_failed", logging.ERROR, reason=type(exc).__name__)
            log_event(
                "upload_discarded",
                session_id=session.session_id,
                frame_id=frame.frame_id,
                deleted=deleted,
            )
            return
        log_event(
            "photo_ready",
            session_id=session.session_id,
            frame_id=frame.frame_id,
            state=State.DONE.value,
            expires_at=_iso_jst(result.expires_at),
            latency_ms=int((time.perf_counter() - t0) * 1000),
        )

    # ---- photo / cancel ----

    def photo(self, session_id: str) -> dict[str, Any]:
        session = self.store.get(session_id)
        with session.lock:
            if session.state == State.DONE and session.photo is not None:
                p = session.photo
                return {
                    "status": "ready",
                    "photo_url": p.photo_url,
                    "share_url": p.share_url,
                    "expires_at": _iso_jst(p.expires_at),
                }
            if session.state == State.UPLOADING:
                return {"status": "pending"}
            if session.photo_error:
                return {"status": "error", "reason": session.photo_error}
            return {"status": "error", "reason": "not_requested"}

    def cancel(self, session_id: str) -> dict[str, Any]:
        session = self.store.get(session_id)
        with session.lock:
            session.generation += 1
            session.discard_frames()  # 未公開の候補を破棄
            session.state = State.CANCELLED
        log_event("session_cancel", session_id=session_id, state=State.CANCELLED.value)
        return {"ok": True}

    # ---- housekeeping ----

    def sweep(self) -> None:
        for sid in self.store.sweep():
            log_event("session_expired", session_id=sid)
