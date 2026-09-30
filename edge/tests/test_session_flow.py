"""hello → start → compose → capture → accepted/timeout → review → photo を API 経由で通す。"""

from __future__ import annotations

import json
import logging
import threading
from concurrent.futures import ThreadPoolExecutor
from urllib.parse import urlparse

import numpy as np
import pytest
from conftest import (
    AUTH,
    FakeAnalyzer,
    FakeMonotonic,
    face,
    frame_headers,
    jpeg_body,
    new_session_id,
    rgb565_body,
    two_faces,
)
from fastapi.testclient import TestClient

from edge.api import create_app
from edge.session import PhotoboothService, SessionStore, State


def start(client: TestClient) -> str:
    sid = new_session_id()
    r = client.post("/v1/sessions", json={"session_id": sid, "started_at_ms": 0}, headers=AUTH)
    assert r.status_code == 200 and r.json() == {"ok": True}
    return sid


def send(client: TestClient, sid: str, frame_id: int, phase: str, **kw) -> dict:
    fmt = kw.pop("fmt", "jpeg")
    body = rgb565_body() if fmt == "rgb565" else jpeg_body()
    r = client.post(
        f"/v1/sessions/{sid}/frames",
        content=body,
        headers=frame_headers(frame_id, phase, fmt=fmt, **kw),
    )
    assert r.status_code == 200, r.text
    return r.json()


def test_hello(client: TestClient) -> None:
    r = client.post(
        "/v1/hello", json={"device_id": "stackchan-01", "protocol_version": 1}, headers=AUTH
    )
    assert r.status_code == 200
    assert r.json() == {"ready": True, "edge_state": "idle", "max_faces": 4, "countdown_sec": 10}
    r = client.post("/v1/hello", json={"device_id": "x", "protocol_version": 2}, headers=AUTH)
    assert r.status_code == 400


def test_accepted_flow_to_photo_ready(client: TestClient, analyzer: FakeAnalyzer, service) -> None:
    sid = start(client)
    # COMPOSE: 2 人が安定 → 目標 2
    analyzer.push(two_faces(smile=0.1), two_faces(smile=0.1))
    r1 = send(client, sid, 1, "compose")
    r2 = send(client, sid, 2, "compose")
    assert r2["target_face_count"] == 2 and not r2["accepted"]
    # CAPTURE: 笑っていない → 笑顔 1 枚目 (未採用) → 2 枚目で採用
    analyzer.push(two_faces(smile=0.1), two_faces(), two_faces())
    r3 = send(client, sid, 3, "capture")
    assert not r3["all_smiling"] and not r3["accepted"]
    r4 = send(client, sid, 4, "capture")
    assert r4["all_smiling"] and not r4["accepted"]
    r5 = send(client, sid, 5, "capture")
    assert r5["accepted"] is True
    assert set(r5) == {
        "session_id", "frame_id", "dropped", "face_count", "target_face_count", "all_in_frame",
        "all_eyes_open", "all_smiling", "servo_dx", "servo_dy", "hint", "accepted", "latency_ms",
    }  # fmt: skip
    assert r1["dropped"] is False and r5["frame_id"] == 5 and r5["session_id"] == sid

    session = service.store.get(sid)
    assert session.accepted.frame_id == 5
    assert session.accepted.raw == jpeg_body()  # 判定に使った同じバイト列を保持

    # 採用後のフレームは処理しない
    calls = analyzer.calls
    r6 = send(client, sid, 6, "capture")
    assert r6["dropped"] is True and r6["accepted"] is False and analyzer.calls == calls

    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202 and r.json() == {"status": "uploading"}
    r = client.get(f"/v1/sessions/{sid}/photo", headers=AUTH)
    body = r.json()
    assert body["status"] == "ready"
    assert body["photo_url"].startswith("http://edge.test:8765/mock/p/")
    assert body["share_url"] == "http://edge.test:8765/mock/share/x"
    assert body["expires_at"] == "2026-09-30T22:00:00+09:00"  # 12:00Z + 60 分 (JST)
    assert session.accepted is None  # 公開後は edge にバイト列を残さない

    # save の再送は冪等で同じ写真
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json() == body

    # モックページで JPEG が見える
    path = body["photo_url"].removeprefix("http://edge.test:8765")
    page = client.get(path)
    assert page.status_code == 200 and "写真をダウンロード" in page.text
    img = client.get(path + ".jpg")
    assert img.status_code == 200 and img.content[:2] == b"\xff\xd8"


def test_rgb565_frames(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    analyzer.push([face()])
    r = send(client, sid, 1, "capture", fmt="rgb565")
    assert r["face_count"] == 1
    r = client.post(
        f"/v1/sessions/{sid}/frames",
        content=b"\x00" * 10,
        headers=frame_headers(2, "capture", fmt="rgb565"),
    )
    assert r.status_code == 400


def test_target_kept_when_one_leaves(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    analyzer.push(two_faces(), two_faces(), [face()], [face()], [face()], [face()])
    send(client, sid, 1, "compose")
    send(client, sid, 2, "compose")
    results = [send(client, sid, i, "capture") for i in range(3, 7)]
    assert all(r["target_face_count"] == 2 for r in results)
    assert not any(r["accepted"] for r in results)


def test_timeout_candidate_then_retake(client: TestClient, analyzer: FakeAnalyzer, service) -> None:
    sid = start(client)
    analyzer.push(
        [face(smile=0.2)],
        [face(smile=0.4)],  # 最良: 笑顔 0.4
        [face(smile=0.3)],
        [],
    )
    for i in range(1, 5):
        assert not send(client, sid, i, "capture")["accepted"]
    r = client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH)
    assert r.status_code == 200
    cand = r.json()["candidate"]
    assert cand["frame_id"] == 2
    assert cand["reason"].startswith("faces=1 in_frame=1 eyes_open=1 min_smile=0.40")
    assert isinstance(cand["score"], float)
    # 時間切れ後のフレームは処理しない
    assert send(client, sid, 5, "capture")["dropped"] is True

    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "retake"}, headers=AUTH)
    assert r.status_code == 200 and r.json() == {"ok": True}
    session = service.store.get(sid)
    assert session.best is None and session.accepted is None
    assert session.state == State.COMPOSE
    # retake 後は新しい 10 秒。frame_id は振り直してよい
    analyzer.push([face()])
    assert send(client, sid, 1, "compose")["dropped"] is False


def test_timeout_without_faces_has_no_candidate(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    analyzer.push([], [])
    send(client, sid, 1, "capture")
    send(client, sid, 2, "capture")
    r = client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH)
    assert r.json() == {"candidate": None}
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 409


def test_timeout_candidate_can_be_saved(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    analyzer.push([face(smile=0.2)])
    send(client, sid, 1, "capture")
    client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH)
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()["status"] == "ready"


def test_cancel_discards(client: TestClient, analyzer: FakeAnalyzer, service) -> None:
    sid = start(client)
    analyzer.push([face(smile=0.2)])
    send(client, sid, 1, "capture")
    assert service.store.get(sid).best is not None
    r = client.post(f"/v1/sessions/{sid}/cancel", headers=AUTH)
    assert r.status_code == 200 and r.json() == {"ok": True}
    session = service.store.get(sid)
    assert session.best is None and session.state == State.CANCELLED
    assert send(client, sid, 2, "capture")["dropped"] is True


def test_stale_frame_dropped(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    analyzer.push([face()])
    assert send(client, sid, 10, "compose")["dropped"] is False
    calls = analyzer.calls
    assert send(client, sid, 9, "compose")["dropped"] is True
    assert send(client, sid, 10, "compose")["dropped"] is True
    assert analyzer.calls == calls


def test_session_restart_resets(client: TestClient, analyzer: FakeAnalyzer, service) -> None:
    sid = start(client)
    analyzer.push([face(smile=0.1)])
    send(client, sid, 5, "capture")
    r = client.post("/v1/sessions", json={"session_id": sid, "started_at_ms": 0}, headers=AUTH)
    assert r.status_code == 200
    session = service.store.get(sid)
    assert session.last_frame_id == -1 and session.best is None
    assert session.state == State.COMPOSE


def test_unknown_session_404(client: TestClient) -> None:
    sid = new_session_id()
    assert client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH).status_code == 404
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).status_code == 404
    assert client.post(f"/v1/sessions/{sid}/cancel", headers=AUTH).status_code == 404
    r = client.post(
        f"/v1/sessions/{sid}/frames", content=jpeg_body(), headers=frame_headers(1, "capture")
    )
    assert r.status_code == 404


def test_invalid_session_id_and_headers(client: TestClient) -> None:
    r = client.post("/v1/sessions", json={"session_id": "not-a-uuid"}, headers=AUTH)
    assert r.status_code == 400
    sid = start(client)
    h = frame_headers(1, "capture")
    del h["X-Frame-Id"]
    r = client.post(f"/v1/sessions/{sid}/frames", content=jpeg_body(), headers=h)
    assert r.status_code == 400
    h = frame_headers(1, "later")
    r = client.post(f"/v1/sessions/{sid}/frames", content=jpeg_body(), headers=h)
    assert r.status_code == 400


def test_session_expires_after_idle(client: TestClient, service: PhotoboothService, mono) -> None:
    sid = start(client)
    mono.t += 299
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).status_code == 200
    mono.t += 300
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).status_code == 404


def test_upload_failure_then_retry(client: TestClient, analyzer: FakeAnalyzer, service) -> None:
    sid = start(client)
    analyzer.push([face()])
    send(client, sid, 1, "compose")
    send(client, sid, 2, "capture")
    send(client, sid, 3, "capture")
    real = service.gallery.upload
    service.gallery.upload = lambda *a, **k: (_ for _ in ()).throw(RuntimeError("down"))
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json() == {
        "status": "error",
        "reason": "upload_failed",
    }
    service.gallery.upload = real
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()["status"] == "ready"


def test_logs_have_no_bytes_tokens_or_urls(
    client: TestClient, analyzer: FakeAnalyzer, caplog: pytest.LogCaptureFixture
) -> None:
    from edge.logging_setup import JsonFormatter

    caplog.set_level(logging.INFO)
    sid = start(client)
    analyzer.push([face()])
    send(client, sid, 1, "compose")
    send(client, sid, 2, "capture")
    send(client, sid, 3, "capture")
    client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    photo = client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()
    token = photo["photo_url"].rsplit("/", 1)[1]
    client.get(f"/mock/p/{token}")

    fmt = JsonFormatter()
    lines = [fmt.format(r) for r in caplog.records]
    events = [json.loads(line)["event"] for line in lines]
    assert "frame" in events and "photo_ready" in events
    text = "\n".join(lines)
    assert token not in text
    assert "http://" not in text and "https://" not in text
    assert "\\xff\\xd8" not in text and "test-key" not in text
    ready = next(json.loads(x) for x in lines if json.loads(x)["event"] == "photo_ready")
    assert ready["expires_at"] == "2026-09-30T22:00:00+09:00"


class SlowGallery:
    """upload() が写真を登録したあと、release されるまで戻らない gallery。"""

    def __init__(self, inner) -> None:
        self.inner = inner
        self.uploaded = threading.Event()
        self.release = threading.Event()
        self.results: list = []

    def upload(self, jpeg, session_id, captured_at):
        res = self.inner.upload(jpeg, session_id, captured_at)
        self.results.append(res)
        self.uploaded.set()
        assert self.release.wait(5)
        return res

    def delete(self, photo_id: str) -> None:
        self.inner.delete(photo_id)


@pytest.mark.parametrize("interrupt", ["cancel", "restart", "sweep"])
def test_upload_finished_after_interrupt_is_deleted(cfg, gallery, clock, interrupt) -> None:
    analyzer = FakeAnalyzer([[face()]])
    slow = SlowGallery(gallery)
    mono = FakeMonotonic()
    executor = ThreadPoolExecutor(1)
    service = PhotoboothService(
        cfg, analyzer, slow, store=SessionStore(clock=mono), executor=executor, wall_clock=clock
    )
    client = TestClient(create_app(service, cfg.auth, gallery, background_sweep=False))
    sid = start(client)
    for i, phase in enumerate(["compose", "capture", "capture"], 1):
        send(client, sid, i, phase)
    r = client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert r.status_code == 202
    assert slow.uploaded.wait(5)
    path = urlparse(slow.results[0].photo_url).path
    assert client.get(path).status_code == 200  # アップロード自体は終わっている

    if interrupt == "cancel":
        client.post(f"/v1/sessions/{sid}/cancel", headers=AUTH)
    elif interrupt == "restart":
        client.post("/v1/sessions", json={"session_id": sid}, headers=AUTH)
    else:
        mono.t += 301
        service.sweep()
    slow.release.set()
    executor.shutdown(wait=True)

    assert client.get(path).status_code in (404, 410)
    assert client.get(path + ".jpg").status_code in (404, 410)


def _retake(client: TestClient, sid: str):
    return client.post(f"/v1/sessions/{sid}/review", json={"decision": "retake"}, headers=AUTH)


def test_retake_only_in_review_or_timeout(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    r = _retake(client, sid)  # COMPOSE
    assert r.status_code == 409 and r.json() == {"error": "nothing_to_retake"}
    analyzer.push([face(smile=0.1)])
    send(client, sid, 1, "capture")
    assert _retake(client, sid).status_code == 409  # CAPTURE

    client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH)
    assert _retake(client, sid).status_code == 200  # REVIEW (候補あり)

    analyzer.push([])
    send(client, sid, 1, "capture")
    client.post(f"/v1/sessions/{sid}/timeout", headers=AUTH)
    assert _retake(client, sid).status_code == 200  # TIMEOUT (候補なし)

    # 採用 → save → DONE では撮り直せない (device は新しいセッションを始める)
    analyzer.push([face()], [face()], [face()])
    for i, phase in enumerate(["compose", "capture", "capture"], 1):
        send(client, sid, i, phase)
    client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()["status"] == "ready"
    assert _retake(client, sid).status_code == 409
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()["status"] == "ready"

    client.post(f"/v1/sessions/{sid}/cancel", headers=AUTH)
    assert _retake(client, sid).status_code == 409  # CANCELLED


def test_retake_rejected_while_uploading(cfg, gallery, clock) -> None:
    slow = SlowGallery(gallery)
    executor = ThreadPoolExecutor(1)
    service = PhotoboothService(
        cfg,
        FakeAnalyzer([[face()]]),
        slow,
        store=SessionStore(clock=FakeMonotonic()),
        executor=executor,
        wall_clock=clock,
    )
    client = TestClient(create_app(service, cfg.auth, gallery, background_sweep=False))
    sid = start(client)
    for i, phase in enumerate(["compose", "capture", "capture"], 1):
        send(client, sid, i, phase)
    client.post(f"/v1/sessions/{sid}/review", json={"decision": "save"}, headers=AUTH)
    assert slow.uploaded.wait(5)
    assert _retake(client, sid).status_code == 409
    slow.release.set()
    executor.shutdown(wait=True)
    assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).json()["status"] == "ready"


class _HookLock:
    """acquire の直前に 1 度だけ hook を呼ぶロック (掃除と get の競合を再現する)。"""

    def __init__(self, hook) -> None:
        self._lock = threading.Lock()
        self._hook = hook

    def __enter__(self):
        hook, self._hook = self._hook, None
        if hook:
            hook()
        return self._lock.__enter__()

    def __exit__(self, *exc):
        return self._lock.__exit__(*exc)


def test_sweep_rechecks_touched_under_session_lock(client: TestClient, service, mono) -> None:
    sid = start(client)
    store = service.store
    mono.t += 301  # 期限切れの候補になる
    session = store._sessions[sid]
    # 掃除が候補を選んだあと、セッションを消す前に get() が来る
    session.lock = _HookLock(lambda: store.get(sid))
    assert store.sweep() == []
    assert store.get(sid) is session


def test_get_touches_under_store_lock(service, mono) -> None:
    """touched の更新は store のロックの中で行う (掃除との間に隙間を作らない)。"""
    store = service.store
    sid = new_session_id()
    store.start(sid)
    held: list[bool] = []

    def clock() -> float:
        held.append(store._lock.locked())
        return mono.t

    store._clock = clock
    store.get(sid)
    assert held == [True]


def test_frame_requires_content_length(client: TestClient, analyzer: FakeAnalyzer) -> None:
    sid = start(client)
    body = jpeg_body()

    def chunks():
        yield body[:100]
        yield body[100:]

    h = frame_headers(1, "capture")
    r = client.post(f"/v1/sessions/{sid}/frames", content=chunks(), headers=h)
    assert r.status_code == 411 and r.json() == {"error": "length_required"}
    assert analyzer.calls == 0


def test_frame_too_large_declared_or_streamed(client: TestClient, analyzer: FakeAnalyzer) -> None:
    from edge.api import MAX_FRAME_BYTES

    sid = start(client)
    h = {**frame_headers(1, "capture"), "Content-Length": str(MAX_FRAME_BYTES + 1)}
    r = client.post(f"/v1/sessions/{sid}/frames", content=b"x", headers=h)
    assert r.status_code == 413 and r.json() == {"error": "frame_too_large"}
    # Content-Length を小さく偽っても、読みながら上限で打ち切る
    h = {**frame_headers(2, "capture"), "Content-Length": "10"}
    big = b"\0" * (MAX_FRAME_BYTES + 4096)
    r = client.post(f"/v1/sessions/{sid}/frames", content=big, headers=h)
    assert r.status_code == 413
    assert analyzer.calls == 0


def test_session_count_is_capped(
    client: TestClient, service, mono, cfg, caplog: pytest.LogCaptureFixture
) -> None:
    caplog.set_level(logging.INFO)
    assert cfg.server.max_sessions == 8
    sids = []
    for _ in range(cfg.server.max_sessions + 1):
        sids.append(start(client))
        mono.t += 1
    assert len(service.store) == cfg.server.max_sessions
    # 一番長くイベントの無いセッションが追い出される
    assert client.get(f"/v1/sessions/{sids[0]}/photo", headers=AUTH).status_code == 404
    for sid in sids[1:]:
        assert client.get(f"/v1/sessions/{sid}/photo", headers=AUTH).status_code == 200
    evicted = [r for r in caplog.records if getattr(r, "event", None) == "session_evicted"]
    assert len(evicted) == 1 and evicted[0].fields["session_id"] == sids[0]


def test_image_too_large(client: TestClient, analyzer: FakeAnalyzer, cfg) -> None:
    from edge.image import encode_jpeg

    assert (cfg.capture.max_width, cfg.capture.max_height) == (1280, 960)
    sid = start(client)
    # 宣言サイズが上限超え (RGB565)
    h = frame_headers(1, "capture", fmt="rgb565", width=1281, height=10)
    r = client.post(f"/v1/sessions/{sid}/frames", content=b"\0" * (1281 * 10 * 2), headers=h)
    assert r.status_code == 400 and r.json() == {"error": "image_too_large"}
    # JPEG の実サイズが上限超え
    big = encode_jpeg(np.zeros((970, 16, 3), dtype=np.uint8))
    h = frame_headers(2, "capture", fmt="jpeg", width=16, height=970)
    r = client.post(f"/v1/sessions/{sid}/frames", content=big, headers=h)
    assert r.status_code == 400 and r.json() == {"error": "image_too_large"}
    assert analyzer.calls == 0
