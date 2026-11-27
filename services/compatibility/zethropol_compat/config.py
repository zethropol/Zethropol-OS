"""XDG paths, profile schema, validation, and canonical templates for zethropol-compatibility."""

from __future__ import annotations

from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
from typing import Any

SCHEMA_VERSION = 1
PROFILE_METADATA_FILE = "zethropol-profile.json"
PROFILE_NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,47}$")
VALID_RENDERERS = ("auto", "dxvk", "wined3d")
SUPPORTED_TARGET_EXTENSIONS = (".exe", ".msi", ".lnk")


class CompatibilityError(RuntimeError):
    """Structured fail-closed error for compatibility operations."""


@dataclass(frozen=True)
class ProfileTemplate:
    name: str
    requested_arch: str
    windows_version: str
    default_renderer: str
    locale: str
    sandbox: bool
    description: str


TEMPLATES: dict[str, ProfileTemplate] = {
    "default-win64": ProfileTemplate(
        name="default-win64",
        requested_arch="win64",
        windows_version="win10",
        default_renderer="auto",
        locale="tr_TR.UTF-8",
        sandbox=True,
        description="Default 64-bit Windows 10 isolated compatibility profile",
    ),
    "classic-win32": ProfileTemplate(
        name="classic-win32",
        requested_arch="win32",
        windows_version="win7",
        default_renderer="wined3d",
        locale="tr_TR.UTF-8",
        sandbox=True,
        description="Classic 32-bit Windows 7 compatibility profile (auto-adapts to WoW64 on Wine 11+)",
    ),
}


@dataclass
class ProfileMetadata:
    schema_version: int
    profile: str
    template: str
    requested_arch: str
    effective_arch: str
    windows_version: str
    renderer: str
    locale: str
    sandbox: bool
    wine_binary: str
    wine_version: str
    created_at: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "ProfileMetadata":
        required = (
            "schema_version",
            "profile",
            "template",
            "requested_arch",
            "effective_arch",
            "windows_version",
            "renderer",
            "locale",
            "sandbox",
            "wine_binary",
            "wine_version",
            "created_at",
        )
        missing = [k for k in required if k not in data]
        if missing:
            raise CompatibilityError(f"Profile metadata missing fields: {', '.join(missing)}")
        profile = validate_profile_name(str(data["profile"]))
        renderer = validate_renderer(str(data["renderer"]))
        return cls(
            schema_version=int(data["schema_version"]),
            profile=profile,
            template=str(data["template"]),
            requested_arch=str(data["requested_arch"]),
            effective_arch=str(data["effective_arch"]),
            windows_version=str(data["windows_version"]),
            renderer=renderer,
            locale=str(data["locale"]),
            sandbox=bool(data["sandbox"]),
            wine_binary=str(data["wine_binary"]),
            wine_version=str(data["wine_version"]),
            created_at=str(data["created_at"]),
        )


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def xdg_data_home() -> Path:
    raw = os.environ.get("XDG_DATA_HOME", "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home() / ".local" / "share"


def xdg_config_home() -> Path:
    raw = os.environ.get("XDG_CONFIG_HOME", "").strip()
    if raw:
        return Path(raw).expanduser()
    return Path.home() / ".config"


def prefixes_root() -> Path:
    return xdg_data_home() / "zethropol" / "wine-prefixes"


def applications_dir() -> Path:
    return xdg_data_home() / "applications"


def icons_dir() -> Path:
    return xdg_data_home() / "icons" / "hicolor" / "256x256" / "apps"


def validate_profile_name(name: str) -> str:
    if not isinstance(name, str) or not PROFILE_NAME_RE.fullmatch(name):
        raise CompatibilityError(
            f"Invalid profile name {name!r}: must match ^[a-z0-9][a-z0-9_-]{{0,47}}$"
        )
    return name


def validate_renderer(renderer: str) -> str:
    value = renderer.strip().lower()
    if value not in VALID_RENDERERS:
        raise CompatibilityError(
            f"Unsupported renderer {renderer!r}: choose one of {', '.join(VALID_RENDERERS)}"
        )
    return value


def get_template(name: str) -> ProfileTemplate:
    if name not in TEMPLATES:
        raise CompatibilityError(
            f"Unknown profile template {name!r}: choose one of {', '.join(sorted(TEMPLATES))}"
        )
    return TEMPLATES[name]


def prefix_dir(profile: str) -> Path:
    clean = validate_profile_name(profile)
    root = prefixes_root().resolve()
    candidate = (root / clean).resolve()
    if candidate.parent != root:
        raise CompatibilityError(f"Profile path escaped prefixes root: {candidate}")
    return candidate


def metadata_path(prefix_path: Path) -> Path:
    return prefix_path / PROFILE_METADATA_FILE


def write_profile_metadata(prefix_path: Path, meta: ProfileMetadata) -> Path:
    path = metadata_path(prefix_path)
    tmp = prefix_path / f".{PROFILE_METADATA_FILE}.tmp"
    tmp.write_text(json.dumps(meta.to_dict(), ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.chmod(tmp, 0o600)
    tmp.replace(path)
    return path


def load_profile_metadata(prefix_path: Path) -> ProfileMetadata:
    path = metadata_path(prefix_path)
    if not path.is_file():
        raise CompatibilityError(f"Profile metadata not found: {path}")
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CompatibilityError(f"Invalid profile metadata in {path}: {exc}") from exc
    if not isinstance(raw, dict):
        raise CompatibilityError(f"Profile metadata in {path} must be a JSON object")
    return ProfileMetadata.from_dict(raw)
