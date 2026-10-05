from __future__ import annotations

import pytest
from conftest import AUTH, DEVICE_ID, DEVICE_KEY, EDGE_DIR, new_session_id
from fastapi.testclient import TestClient

from edge.auth import verify_device
from edge.config import AuthConfig, ConfigError, load_config

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


def test_env_device_id_overrides_config(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[auth]\ndevice_id = "from-file"\n')
    assert load_config(p, env={}).auth.device_id == "from-file"
    assert load_config(p, env={"EDGE_DEVICE_ID": "from-env"}).auth.device_id == "from-env"
    # 空文字は未設定として扱う (コンテナに空の変数が渡っても設定ファイルの値を使う)
    assert load_config(p, env={"EDGE_DEVICE_ID": ""}).auth.device_id == "from-file"


def test_env_gallery_url_switches_to_http(tmp_path) -> None:
    # edge-cloud のコンテナと同じ: config.example.toml (mock) を環境変数だけで http にする
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "mock"\n')
    env = {"GALLERY_URL": "https://gallery.test", "GALLERY_KEY": "gallery-secret"}
    cfg = load_config(p, env=env)
    assert cfg.gallery.mode == "http"
    assert cfg.gallery.url == "https://gallery.test"
    assert cfg.gallery.key == "gallery-secret"
    assert load_config(p, env={}).gallery.mode == "mock"


def test_env_gallery_url_is_validated(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "mock"\n')
    with pytest.raises(ConfigError, match="https"):
        load_config(p, env={"GALLERY_URL": "http://gallery.test", "GALLERY_KEY": "k"})
    with pytest.raises(ConfigError, match="GALLERY_KEY"):
        load_config(p, env={"GALLERY_URL": "https://gallery.test"})


def test_env_gallery_url_completes_file_config(tmp_path) -> None:
    # ファイル単体では不完全 (http なのに url なし) でも、環境変数で補えれば通す
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "http"\n')
    env = {"GALLERY_URL": "https://gallery.test", "GALLERY_KEY": "k"}
    assert load_config(p, env=env).gallery.url == "https://gallery.test"
    with pytest.raises(ConfigError, match="url is required"):
        load_config(p, env={"GALLERY_KEY": "k"})


def test_env_gallery_url_replaces_invalid_file_url(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "http"\nurl = "http://old.example"\n')
    env = {"GALLERY_URL": "https://gallery.test", "GALLERY_KEY": "k"}
    assert load_config(p, env=env).gallery.url == "https://gallery.test"
    with pytest.raises(ConfigError, match="https"):
        load_config(p, env={"GALLERY_KEY": "k"})


def test_require_device_key_rejects_example_key() -> None:
    # edge/Dockerfile は EDGE_REQUIRE_DEVICE_KEY=1。鍵を渡し忘れたら change-me で listen しない
    example = EDGE_DIR / "config.example.toml"
    with pytest.raises(ConfigError, match="EDGE_DEVICE_KEY"):
        load_config(example, env={"EDGE_REQUIRE_DEVICE_KEY": "1"})
    env = {"EDGE_REQUIRE_DEVICE_KEY": "1", "EDGE_DEVICE_KEY": "device-secret"}
    assert load_config(example, env=env).auth.device_key == "device-secret"
    # PC では従来どおり警告だけで起動する
    assert load_config(example, env={}).auth.device_key == "change-me"
    assert load_config(example, env={"EDGE_REQUIRE_DEVICE_KEY": "0"}).auth.device_key == "change-me"


def test_example_config_with_container_env() -> None:
    # edge/Dockerfile は config.example.toml を config.toml として焼き込み、残りは環境変数で渡す
    env = {
        "EDGE_DEVICE_KEY": "device-secret",
        "EDGE_DEVICE_ID": "stackchan-02",
        "GALLERY_URL": "https://gallery.test",
        "GALLERY_KEY": "gallery-secret",
    }
    cfg = load_config(EDGE_DIR / "config.example.toml", env=env)
    assert cfg.auth == AuthConfig(device_id="stackchan-02", device_key="device-secret")
    assert (cfg.gallery.mode, cfg.gallery.url) == ("http", "https://gallery.test")
    assert cfg.server.host == "0.0.0.0"
    assert cfg.server.port == 8765


def test_http_gallery_reads_key_from_env(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "http"\nurl = "https://gallery.test"\n')
    cfg = load_config(p, env={"GALLERY_KEY": "gallery-secret"})
    assert cfg.gallery.mode == "http"
    assert cfg.gallery.url == "https://gallery.test"
    assert cfg.gallery.key == "gallery-secret"


def test_http_gallery_requires_env_key(tmp_path) -> None:
    p = tmp_path / "config.toml"
    p.write_text('[gallery]\nmode = "http"\nurl = "https://gallery.test"\n')
    with pytest.raises(ConfigError, match="GALLERY_KEY"):
        load_config(p, env={})
