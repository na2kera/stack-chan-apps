from __future__ import annotations

import pytest
from conftest import AUTH, DEVICE_ID, DEVICE_KEY, new_session_id
from fastapi.testclient import TestClient

from edge.auth import verify_device
from edge.config import AuthConfig, load_config

HELLO = {"device_id": DEVICE_ID, "protocol_version": 1}


def test_verify_device() -> None:
    cfg = AuthConfig(device_id="a", device_key="k")
    assert verify_device("a", "k", cfg)
    assert not verify_device("a", "x", cfg)
    assert not verify_device("b", "k", cfg)
    assert not verify_device(None, "k", cfg)
    assert not verify_device("a", "", cfg)


@pytest.mark.parametrize(
    "headers",
    [
        {},
        {"X-Device-Id": DEVICE_ID},
        {"X-Device-Id": DEVICE_ID, "X-Device-Key": "wrong"},
        {"X-Device-Id": "other", "X-Device-Key": DEVICE_KEY},
    ],
)
def test_401_on_every_endpoint(client: TestClient, headers: dict) -> None:
    sid = new_session_id()
    calls = [
        ("post", "/v1/hello", {"json": HELLO}),
        ("post", "/v1/sessions", {"json": {"session_id": sid}}),
        ("post", f"/v1/sessions/{sid}/frames", {"content": b"x"}),
        ("post", f"/v1/sessions/{sid}/timeout", {}),
        ("post", f"/v1/sessions/{sid}/review", {"json": {"decision": "save"}}),
        ("get", f"/v1/sessions/{sid}/photo", {}),
        ("get", f"/v1/sessions/{sid}/candidate", {}),
        ("post", f"/v1/sessions/{sid}/cancel", {}),
    ]
    for method, path, kw in calls:
        r = getattr(client, method)(path, headers=headers, **kw)
        assert r.status_code == 401, path


def test_401_before_body_validation(client: TestClient) -> None:
    r = client.post("/v1/hello", json={"bad": 1}, headers={"X-Device-Id": DEVICE_ID})
    assert r.status_code == 401
    r = client.post("/v1/hello", json={"bad": 1}, headers=AUTH)
    assert r.status_code == 400


def test_env_key_overrides_config(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[auth]\ndevice_id = "d"\ndevice_key = "from-file"\n')
    assert load_config(p, env={}).auth.device_key == "from-file"
    assert load_config(p, env={"EDGE_DEVICE_KEY": "from-env"}).auth.device_key == "from-env"
