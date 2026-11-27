"""Pure-Python MS-SHLLINK (.lnk) parser and safe FreeDesktop .desktop shortcut generator."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
from typing import Any

from .config import (
    CompatibilityError,
    applications_dir,
    icons_dir,
    prefix_dir,
    validate_profile_name,
)

LNK_HEADER_SIZE = 0x4C
LNK_CLSID = bytes.fromhex("0114020000000000c000000000000046")

FLAG_HAS_IDLIST = 0x00000001
FLAG_HAS_LINK_INFO = 0x00000002
FLAG_HAS_NAME = 0x00000004
FLAG_HAS_RELATIVE_PATH = 0x00000008
FLAG_HAS_WORKING_DIR = 0x00000010
FLAG_HAS_ARGUMENTS = 0x00000020
FLAG_HAS_ICON_LOCATION = 0x00000040
FLAG_IS_UNICODE = 0x00000080

ALLOWED_SHORTCUT_SUFFIXES = (".exe", ".msi", ".bat", ".cmd")
WIN_DRIVE_C_RE = re.compile(r"^[Cc]:[/\\]+(.*)$")


@dataclass(frozen=True)
class LnkInfo:
    source_path: str
    name: str
    target_windows_path: str
    relative_path: str
    working_dir: str
    arguments: str
    icon_location: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _read_cstr_ansi(buf: bytes, offset: int) -> str:
    if offset <= 0 or offset >= len(buf):
        return ""
    end = buf.find(b"\x00", offset)
    if end < 0:
        end = len(buf)
    return buf[offset:end].decode("cp1254", errors="replace")


def _read_cstr_utf16(buf: bytes, offset: int) -> str:
    if offset <= 0 or offset + 1 >= len(buf):
        return ""
    pos = offset
    while pos + 1 < len(buf):
        if buf[pos] == 0 and buf[pos + 1] == 0:
            break
        pos += 2
    return buf[offset:pos].decode("utf-16le", errors="replace")


def _read_string_data(data: bytes, pos: int, is_unicode: bool) -> tuple[str, int]:
    if pos + 2 > len(data):
        raise CompatibilityError("Truncated StringData length in .lnk file.")
    count = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    byte_len = count * 2 if is_unicode else count
    if pos + byte_len > len(data):
        raise CompatibilityError("Truncated StringData payload in .lnk file.")
    raw = data[pos : pos + byte_len]
    pos += byte_len
    text = raw.decode("utf-16le" if is_unicode else "cp1254", errors="replace")
    return (text, pos)


def parse_lnk(path: Path) -> LnkInfo:
    """Parse an MS-SHLLINK (.lnk) binary file using pure Python stdlib."""
    if not path.is_file():
        raise CompatibilityError(f"Shortcut file not found: {path}")
    data = path.read_bytes()
    if len(data) < LNK_HEADER_SIZE:
        raise CompatibilityError(f"File too small to be a valid .lnk shortcut: {path}")

    header_size = struct.unpack_from("<I", data, 0)[0]
    clsid = data[4:20]
    if header_size != LNK_HEADER_SIZE or clsid != LNK_CLSID:
        raise CompatibilityError(f"Invalid MS-SHLLINK header or CLSID in {path}")

    flags = struct.unpack_from("<I", data, 20)[0]
    is_unicode = bool(flags & FLAG_IS_UNICODE)
    pos = LNK_HEADER_SIZE

    if flags & FLAG_HAS_IDLIST:
        if pos + 2 > len(data):
            raise CompatibilityError(f"Truncated LinkTargetIDList in {path}")
        idlist_size = struct.unpack_from("<H", data, pos)[0]
        pos += 2 + idlist_size
        if pos > len(data):
            raise CompatibilityError(f"LinkTargetIDList exceeds file bounds in {path}")

    local_base_path = ""
    common_suffix = ""
    if flags & FLAG_HAS_LINK_INFO:
        if pos + 4 > len(data):
            raise CompatibilityError(f"Truncated LinkInfo in {path}")
        link_info_size = struct.unpack_from("<I", data, pos)[0]
        if link_info_size < 0x1C or pos + link_info_size > len(data):
            raise CompatibilityError(f"Invalid LinkInfo size in {path}")
        li = data[pos : pos + link_info_size]
        li_header_size, li_flags = struct.unpack_from("<II", li, 4)
        local_base_off = struct.unpack_from("<I", li, 16)[0]
        common_suffix_off = struct.unpack_from("<I", li, 24)[0]
        if li_flags & 0x01:
            if li_header_size >= 0x24 and len(li) >= 0x24:
                uni_base_off, uni_suffix_off = struct.unpack_from("<II", li, 28)
                local_base_path = _read_cstr_utf16(li, uni_base_off) or _read_cstr_ansi(li, local_base_off)
                common_suffix = _read_cstr_utf16(li, uni_suffix_off) or _read_cstr_ansi(li, common_suffix_off)
            else:
                local_base_path = _read_cstr_ansi(li, local_base_off)
                common_suffix = _read_cstr_ansi(li, common_suffix_off)
        pos += link_info_size

    name = ""
    relative_path = ""
    working_dir = ""
    arguments = ""
    icon_location = ""

    if flags & FLAG_HAS_NAME:
        name, pos = _read_string_data(data, pos, is_unicode)
    if flags & FLAG_HAS_RELATIVE_PATH:
        relative_path, pos = _read_string_data(data, pos, is_unicode)
    if flags & FLAG_HAS_WORKING_DIR:
        working_dir, pos = _read_string_data(data, pos, is_unicode)
    if flags & FLAG_HAS_ARGUMENTS:
        arguments, pos = _read_string_data(data, pos, is_unicode)
    if flags & FLAG_HAS_ICON_LOCATION:
        icon_location, pos = _read_string_data(data, pos, is_unicode)

    target = local_base_path
    if target and common_suffix:
        target = target.rstrip("\\") + "\\" + common_suffix.lstrip("\\")

    return LnkInfo(
        source_path=str(path),
        name=name.strip(),
        target_windows_path=target.strip(),
        relative_path=relative_path.strip(),
        working_dir=working_dir.strip(),
        arguments=arguments.strip(),
        icon_location=icon_location.strip(),
    )


def _contains_unsafe_control(text: str) -> bool:
    return any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in text)


def resolve_lnk_target_in_prefix(lnk: LnkInfo, prefix_path: Path) -> Path:
    """Resolve a parsed .lnk target strictly inside <prefix>/drive_c/."""
    drive_c = (prefix_path / "drive_c").resolve()
    if not drive_c.is_dir():
        raise CompatibilityError(f"Prefix drive_c directory missing: {drive_c}")

    candidate: Path | None = None
    if lnk.target_windows_path:
        if _contains_unsafe_control(lnk.target_windows_path):
            raise CompatibilityError("Shortcut target contains unsafe control characters.")
        m = WIN_DRIVE_C_RE.match(lnk.target_windows_path)
        if not m:
            raise CompatibilityError(
                f"Shortcut target '{lnk.target_windows_path}' is outside C:\\ and rejected."
            )
        rel_parts = [p for p in re.split(r"[/\\]+", m.group(1)) if p]
        if any(p in (".", "..") for p in rel_parts):
            raise CompatibilityError(f"Shortcut target contains path traversal: {lnk.target_windows_path}")
        candidate = (drive_c / Path(*rel_parts)).resolve()
    elif lnk.relative_path:
        if _contains_unsafe_control(lnk.relative_path):
            raise CompatibilityError("Shortcut relative_path contains unsafe control characters.")
        rel_unix = lnk.relative_path.replace("\\", "/")
        candidate = (Path(lnk.source_path).resolve().parent / rel_unix).resolve()
    else:
        raise CompatibilityError(f"Shortcut contains neither LinkInfo path nor relative_path: {lnk.source_path}")

    try:
        candidate.relative_to(drive_c)
    except ValueError as exc:
        raise CompatibilityError(f"Resolved shortcut target escapes prefix drive_c: {candidate}") from exc

    if candidate.suffix.lower() not in ALLOWED_SHORTCUT_SUFFIXES:
        raise CompatibilityError(f"Unsupported shortcut target extension: {candidate.name}")
    if not candidate.is_file():
        raise CompatibilityError(f"Resolved shortcut target does not exist: {candidate}")
    return candidate


def escape_desktop_exec_arg(arg: str) -> str:
    """Escape a single argument according to FreeDesktop Desktop Entry Exec quoting rules."""
    if _contains_unsafe_control(arg):
        raise CompatibilityError("Control characters are not allowed in Exec arguments.")
    escaped = arg.replace("\\", "\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$").replace("%", "%%")
    return f'"{escaped}"'


def sanitize_desktop_text(text: str, fallback: str) -> str:
    cleaned = re.sub(r"[\x00-\x1f\x7f]+", " ", text).strip()
    return cleaned or fallback


def slugify_name(name: str, unique_seed: str) -> str:
    base = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")[:32] or "app"
    digest = hashlib.sha256(unique_seed.encode("utf-8")).hexdigest()[:8]
    return f"{base}-{digest}"


def _extract_icon_if_available(target_exe: Path, icon_slug: str) -> str:
    wrestool = shutil.which("wrestool")
    if not wrestool or target_exe.suffix.lower() != ".exe":
        return "application-x-ms-dos-executable"
    out_dir = icons_dir()
    out_dir.mkdir(parents=True, exist_ok=True)
    ico_path = out_dir / f"{icon_slug}.ico"
    try:
        res = subprocess.run(
            [wrestool, "-x", "-t", "14", str(target_exe)],
            capture_output=True,
            timeout=5,
            check=False,
        )
        if res.returncode == 0 and len(res.stdout) >= 22:
            ico_path.write_bytes(res.stdout)
            os.chmod(ico_path, 0o644)
            return str(ico_path)
    except (OSError, subprocess.TimeoutExpired):
        pass
    return "application-x-ms-dos-executable"


def convert_lnk_to_desktop(
    lnk_path: Path,
    profile: str,
    runner_bin: str = "/usr/bin/zethropol-wine-run",
    output_dir: Path | None = None,
) -> Path:
    clean_profile = validate_profile_name(profile)
    pfx = prefix_dir(clean_profile)
    lnk = parse_lnk(lnk_path)
    target = resolve_lnk_target_in_prefix(lnk, pfx)

    display_name = sanitize_desktop_text(lnk.name or lnk_path.stem, target.stem)
    slug = slugify_name(display_name, f"{clean_profile}:{target}")
    icon_value = _extract_icon_if_available(target, f"zethropol-wine-{clean_profile}-{slug}")

    exec_parts = [
        escape_desktop_exec_arg(runner_bin),
        "--profile",
        escape_desktop_exec_arg(clean_profile),
        "--",
        escape_desktop_exec_arg(str(target)),
    ]
    if lnk.arguments:
        # Reject suspicious newlines/control chars; pass tokenized arguments safely
        if _contains_unsafe_control(lnk.arguments):
            raise CompatibilityError("Shortcut arguments contain unsafe control characters.")
        for tok in lnk.arguments.split():
            exec_parts.append(escape_desktop_exec_arg(tok))

    dest_dir = output_dir if output_dir is not None else applications_dir()
    dest_dir.mkdir(parents=True, exist_ok=True)
    desktop_path = dest_dir / f"zethropol-wine-{clean_profile}-{slug}.desktop"

    content = "\n".join([
        "[Desktop Entry]",
        "Type=Application",
        "Version=1.0",
        f"Name={display_name}",
        f"Comment=Windows application in Zethropol profile '{clean_profile}'",
        f"Exec={' '.join(exec_parts)}",
        f"Icon={icon_value}",
        "Terminal=false",
        "Categories=Utility;X-Zethropol-Compatibility;",
        "StartupNotify=true",
        "X-Zethropol-Managed=true",
        f"X-Zethropol-Profile={clean_profile}",
        "",
    ])
    desktop_path.write_text(content, encoding="utf-8")
    os.chmod(desktop_path, 0o644)
    return desktop_path


def sync_prefix_shortcuts(
    profile: str,
    runner_bin: str = "/usr/bin/zethropol-wine-run",
    output_dir: Path | None = None,
) -> dict[str, Any]:
    clean_profile = validate_profile_name(profile)
    pfx = prefix_dir(clean_profile)
    drive_c = pfx / "drive_c"
    if not drive_c.is_dir():
        raise CompatibilityError(f"Prefix drive_c not found for profile '{clean_profile}': {drive_c}")

    scan_dirs: list[Path] = []
    users_dir = drive_c / "users"
    if users_dir.is_dir():
        for user_entry in sorted(users_dir.iterdir()):
            if not user_entry.is_dir() or user_entry.is_symlink():
                continue
            for rel in (
                "Desktop",
                "AppData/Roaming/Microsoft/Windows/Start Menu/Programs",
                "Start Menu/Programs",
            ):
                cand = user_entry / rel
                if cand.is_dir() and not cand.is_symlink():
                    scan_dirs.append(cand)
    prog_data = drive_c / "ProgramData" / "Microsoft" / "Windows" / "Start Menu" / "Programs"
    if prog_data.is_dir() and not prog_data.is_symlink():
        scan_dirs.append(prog_data)

    converted: list[str] = []
    skipped: list[dict[str, str]] = []
    for base in scan_dirs:
        for lnk_file in sorted(base.rglob("*.lnk")):
            if lnk_file.is_symlink():
                skipped.append({"path": str(lnk_file), "reason": "symlink .lnk skipped"})
                continue
            try:
                out = convert_lnk_to_desktop(
                    lnk_file,
                    profile=clean_profile,
                    runner_bin=runner_bin,
                    output_dir=output_dir,
                )
                converted.append(str(out))
            except CompatibilityError as exc:
                skipped.append({"path": str(lnk_file), "reason": str(exc)})

    return {
        "profile": clean_profile,
        "converted": converted,
        "skipped": skipped,
    }
