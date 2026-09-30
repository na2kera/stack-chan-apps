"""config.toml (無ければ config.example.toml) と環境変数から設定を読む。"""

from __future__ import annotations

import logging
import os
import tomllib
from dataclasses import dataclass, fields, replace
from pathlib import Path
from typing import Any, Literal

log = logging.getLogger(__name__)

EDGE_DIR = Path(__file__).resolve().parents[2]
DEFAULT_CONFIG = EDGE_DIR / "config.toml"
EXAMPLE_CONFIG = EDGE_DIR / "config.example.toml"
ENV_DEVICE_KEY = "EDGE_DEVICE_KEY"


class ConfigError(ValueError):
    pass


@dataclass(frozen=True)
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765


@dataclass(frozen=True)
class AuthConfig:
    device_id: str = "stackchan-01"
    device_key: str = "change-me"


@dataclass(frozen=True)
class CaptureConfig:
    max_faces: int = 4
    countdown_sec: int = 10
    margin_ratio: float = 0.08
    min_face_width_ratio: float = 0.08
    eye_blink_max: float = 0.25
    mouth_smile_min: float = 0.55
    stable_frames: int = 2
    accept_consecutive: int = 2
    rgb565_byte_order: Literal["little", "big"] = "little"


@dataclass(frozen=True)
class HeadConfig:
    gain_x: float = -0.05
    gain_y: float = 0.05
    deadband_px: int = 16
    step_max: int = 30
    min_interval_ms: int = 500
    search_step: int = 20
    x_min: int = -250
    x_max: int = 250
    y_min: int = 250
    y_max: int = 650


@dataclass(frozen=True)
class AnalysisConfig:
    model_path: str = "models/face_landmarker.task"


@dataclass(frozen=True)
class GalleryConfig:
    mode: str = "mock"
    ttl_minutes: float = 60.0
    public_base_url: str = ""


@dataclass(frozen=True)
class ShareConfig:
    text: str = "スタックチャンに撮ってもらいました！ #スタックチャン #StackChan"


@dataclass(frozen=True)
class Config:
    server: ServerConfig
    auth: AuthConfig
    capture: CaptureConfig
    head: HeadConfig
    analysis: AnalysisConfig
    gallery: GalleryConfig
    share: ShareConfig
    base_dir: Path = EDGE_DIR

    def model_file(self) -> Path:
        p = Path(self.analysis.model_path)
        return p if p.is_absolute() else self.base_dir / p


_SECTIONS: dict[str, type] = {
    "server": ServerConfig,
    "auth": AuthConfig,
    "capture": CaptureConfig,
    "head": HeadConfig,
    "analysis": AnalysisConfig,
    "gallery": GalleryConfig,
    "share": ShareConfig,
}


def _build(cls: type, raw: dict[str, Any], section: str) -> Any:
    known = {f.name: f for f in fields(cls)}
    unknown = set(raw) - set(known)
    if unknown:
        raise ConfigError(f"[{section}] unknown keys: {sorted(unknown)}")
    values: dict[str, Any] = {}
    for name, value in raw.items():
        default = getattr(cls(), name)
        # int の既定値に float を入れた等の取り違えを早めに落とす (bool は int の派生なので除外)
        if isinstance(default, bool) or isinstance(value, bool):
            ok = isinstance(value, bool) and isinstance(default, bool)
        elif isinstance(default, float):
            ok = isinstance(value, int | float)
        else:
            ok = isinstance(value, type(default))
        if not ok:
            raise ConfigError(f"[{section}] {name} must be {type(default).__name__}")
        values[name] = float(value) if isinstance(default, float) else value
    return cls(**values)


def _validate(cfg: Config) -> None:
    c = cfg.capture
    if c.rgb565_byte_order not in ("little", "big"):
        raise ConfigError("[capture] rgb565_byte_order must be 'little' or 'big'")
    if not 1 <= c.max_faces <= 10:
        raise ConfigError("[capture] max_faces must be 1..10")
    if c.stable_frames < 1 or c.accept_consecutive < 1:
        raise ConfigError("[capture] stable_frames / accept_consecutive must be >= 1")
    if not 0 <= c.margin_ratio < 0.5:
        raise ConfigError("[capture] margin_ratio must be 0..0.5")
    h = cfg.head
    if h.x_min > h.x_max or h.y_min > h.y_max:
        raise ConfigError("[head] x_min/x_max, y_min/y_max are reversed")
    if h.step_max < 0 or h.min_interval_ms < 0 or h.deadband_px < 0:
        raise ConfigError("[head] step_max / min_interval_ms / deadband_px must be >= 0")
    if cfg.gallery.mode != "mock":
        raise ConfigError("[gallery] mode: only 'mock' is supported in step 2a")
    if cfg.gallery.ttl_minutes <= 0:
        raise ConfigError("[gallery] ttl_minutes must be > 0")


def parse_config(raw: dict[str, Any], base_dir: Path = EDGE_DIR) -> Config:
    unknown = set(raw) - set(_SECTIONS)
    if unknown:
        raise ConfigError(f"unknown sections: {sorted(unknown)}")
    parts = {name: _build(cls, raw.get(name, {}), name) for name, cls in _SECTIONS.items()}
    cfg = Config(**parts, base_dir=base_dir)
    _validate(cfg)
    return cfg


def load_config(path: Path | None = None, env: dict[str, str] | None = None) -> Config:
    """設定を読む。path 省略時は edge/config.toml、無ければ config.example.toml に警告付きで戻る。

    環境変数 EDGE_DEVICE_KEY があれば [auth] device_key より優先する。
    """
    env = os.environ if env is None else env
    if path is None:
        path = DEFAULT_CONFIG
        if not path.exists():
            log.warning(
                "config.toml not found; falling back to config.example.toml",
                extra={"event": "config_fallback"},
            )
            path = EXAMPLE_CONFIG
    with path.open("rb") as f:
        raw = tomllib.load(f)
    cfg = parse_config(raw, base_dir=path.resolve().parent)
    key = env.get(ENV_DEVICE_KEY)
    if key:
        cfg = replace(cfg, auth=replace(cfg.auth, device_key=key))
    if cfg.auth.device_key == "change-me":
        log.warning(
            "device_key is the example value; set EDGE_DEVICE_KEY or config.toml",
            extra={"event": "config_default_key"},
        )
    return cfg
