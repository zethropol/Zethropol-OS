"""Isolated Wine prefix lifecycle, Turkish locale/font injection, and Z:/$HOME isolation."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import os
from pathlib import Path
import shutil
import subprocess
from typing import Any

from .config import (
    CompatibilityError,
    ProfileMetadata,
    SCHEMA_VERSION,
    applications_dir,
    get_template,
    load_profile_metadata,
    prefix_dir,
    prefixes_root,
    utc_now_iso,
    validate_profile_name,
    validate_renderer,
    write_profile_metadata,
)
from .probe import WineRuntimeInfo, probe_wine

USER_HOME_LINK_DIRS = (
    "Desktop",
    "Documents",
    "Downloads",
    "Music",
    "Pictures",
    "Videos",
    "Templates",
    "My Documents",
    "My Music",
    "My Pictures",
    "My Videos",
)

TURKISH_FONT_SUBSTITUTES: dict[str, str] = {
    "Arial": "Liberation Sans",
    "Arial Tur": "Liberation Sans",
    "Times New Roman": "Liberation Serif",
    "Times New Roman Tur": "Liberation Serif",
    "Courier New": "Liberation Mono",
    "Courier New Tur": "Liberation Mono",
    "Tahoma": "Liberation Sans",
    "Segoe UI": "Liberation Sans",
    "MS Shell Dlg": "Liberation Sans",
    "MS Shell Dlg 2": "DejaVu Sans",
    "Verdana": "DejaVu Sans",
}


@dataclass(frozen=True)
class AuditCheck:
    id: str
    passed: bool
    detail: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class PrefixAuditReport:
    profile: str
    prefix_path: str
    passed: bool
    checks: list[AuditCheck]

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile": self.profile,
            "prefix_path": self.prefix_path,
            "passed": self.passed,
            "checks": [c.to_dict() for c in self.checks],
        }


def resolve_effective_arch(requested_arch: str, wine_info: WineRuntimeInfo) -> tuple[str, str]:
    """Return (WINEARCH_env_value, recorded_effective_arch)."""
    if requested_arch == "win32":
        if wine_info.supports_native_win32:
            return ("win32", "win32")
        return ("win64", "win64-wow64")
    if requested_arch == "win64":
        return ("win64", "win64")
    raise CompatibilityError(f"Unsupported requested Wine architecture: {requested_arch!r}")


def build_zethropol_registry(template_name: str, locale: str = "tr_TR.UTF-8") -> str:
    tpl = get_template(template_name)
    is_win7 = tpl.windows_version == "win7"
    product_name = "Microsoft Windows 7" if is_win7 else "Microsoft Windows 10"
    current_ver = "6.1" if is_win7 else "10.0"
    build_num = "7601" if is_win7 else "19045"
    csd_ver = "Service Pack 1" if is_win7 else ""

    lines: list[str] = [
        "Windows Registry Editor Version 5.00",
        "",
        "[HKEY_CURRENT_USER\\Software\\Wine]",
        f'"Version"="{tpl.windows_version}"',
        "",
        "[HKEY_CURRENT_USER\\Software\\Wine\\DllOverrides]",
        '"winemenubuilder.exe"=""',
        "",
        "[HKEY_CURRENT_USER\\Control Panel\\International]",
        '"Locale"="0000041F"',
        '"LocaleName"="tr-TR"',
        '"sLanguage"="TRK"',
        '"sCountry"="Turkey"',
        '"iCountry"="90"',
        '"sDecimal"=","',
        '"sThousand"="."',
        '"sShortDate"="dd.MM.yyyy"',
        '"sTimeFormat"="HH:mm:ss"',
        f'"ZethropolLocale"="{locale}"',
        "",
        "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion]",
        f'"ProductName"="{product_name}"',
        f'"CurrentVersion"="{current_ver}"',
        f'"CurrentBuild"="{build_num}"',
        f'"CurrentBuildNumber"="{build_num}"',
        f'"CSDVersion"="{csd_ver}"',
        "",
        "[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows NT\\CurrentVersion\\FontSubstitutes]",
    ]
    for win_font, linux_font in TURKISH_FONT_SUBSTITUTES.items():
        lines.append(f'"{win_font}"="{linux_font}"')

    lines.extend([
        "",
        "[HKEY_CURRENT_USER\\Software\\Wine\\Fonts\\Replacements]",
    ])
    for win_font, linux_font in TURKISH_FONT_SUBSTITUTES.items():
        lines.append(f'"{win_font}"="{linux_font}"')

    lines.append("")
    return "\n".join(lines)


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def enforce_prefix_isolation(prefix_path: Path) -> dict[str, Any]:
    """Remove Z:\\ root mapping and replace any $HOME user-folder symlinks with isolated directories."""
    prefix_resolved = prefix_path.resolve()
    removed_drives: list[str] = []
    isolated_dirs: list[str] = []

    os.chmod(prefix_resolved, 0o700)

    dosdevices = prefix_resolved / "dosdevices"
    if dosdevices.is_dir():
        for entry in sorted(dosdevices.iterdir()):
            name_lower = entry.name.lower()
            if name_lower == "c:":
                continue
            if not entry.is_symlink():
                continue
            target_raw = os.readlink(entry)
            target_resolved = (entry.parent / target_raw).resolve()
            # Always remove Z: and any drive pointing to / or outside the prefix
            if name_lower == "z:" or target_resolved == Path("/") or not _is_within(target_resolved, prefix_resolved):
                entry.unlink()
                removed_drives.append(entry.name)

    users_dir = prefix_resolved / "drive_c" / "users"
    if users_dir.is_dir():
        for user_entry in sorted(users_dir.iterdir()):
            if not user_entry.is_dir() or user_entry.is_symlink():
                continue
            for folder in USER_HOME_LINK_DIRS:
                candidate = user_entry / folder
                if candidate.is_symlink():
                    target_raw = os.readlink(candidate)
                    target_resolved = (candidate.parent / target_raw).resolve()
                    if not _is_within(target_resolved, prefix_resolved):
                        candidate.unlink()
                        candidate.mkdir(mode=0o700, exist_ok=True)
                        isolated_dirs.append(str(candidate.relative_to(prefix_resolved)))

    shared_dir = prefix_resolved / "drive_c" / "zethropol-shared"
    shared_dir.mkdir(mode=0o700, parents=True, exist_ok=True)

    return {
        "removed_drives": removed_drives,
        "isolated_user_dirs": isolated_dirs,
    }


def _wine_base_env(prefix_path: Path, winearch_env: str, locale: str) -> dict[str, str]:
    env = os.environ.copy()
    env["WINEPREFIX"] = str(prefix_path)
    env["WINEARCH"] = winearch_env
    env["LANG"] = locale
    env["LC_ALL"] = locale
    env["LC_CTYPE"] = locale
    env["WINEDEBUG"] = "-all"
    env["WINEDLLOVERRIDES"] = "mscoree,mshtml,winemenubuilder.exe="
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    return env


def _wait_wineserver(wine_binary: str, env: dict[str, str], timeout: int = 30) -> None:
    wineserver = Path(wine_binary).parent / "wineserver"
    if not wineserver.is_file():
        found = shutil.which("wineserver")
        wineserver = Path(found) if found else wineserver
    if wineserver.is_file():
        subprocess.run(
            [str(wineserver), "-w"],
            env=env,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )


def init_prefix(
    profile: str,
    template: str = "default-win64",
    renderer: str | None = None,
    sandbox: bool | None = None,
    wine_bin: str | None = None,
    force: bool = False,
) -> tuple[ProfileMetadata, PrefixAuditReport]:
    clean_profile = validate_profile_name(profile)
    tpl = get_template(template)
    eff_renderer = validate_renderer(renderer) if renderer is not None else tpl.default_renderer
    eff_sandbox = tpl.sandbox if sandbox is None else bool(sandbox)

    wine_info = probe_wine(wine_bin)
    winearch_env, effective_arch = resolve_effective_arch(tpl.requested_arch, wine_info)

    pfx = prefix_dir(clean_profile)
    if pfx.exists() and not force:
        raise CompatibilityError(
            f"Profile '{clean_profile}' already exists at {pfx} (pass --force to re-apply/re-init)."
        )

    pfx.parent.mkdir(parents=True, exist_ok=True)
    pfx.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(pfx, 0o700)

    env = _wine_base_env(pfx, winearch_env, tpl.locale)
    wineboot = Path(wine_info.binary).parent / "wineboot"
    boot_cmd = [str(wineboot), "--init"] if wineboot.is_file() else [wine_info.binary, "wineboot", "--init"]

    boot_res = subprocess.run(
        boot_cmd,
        env=env,
        capture_output=True,
        text=True,
        timeout=90,
        check=False,
    )
    if boot_res.returncode != 0:
        raise CompatibilityError(
            f"wineboot --init failed (rc={boot_res.returncode}): {(boot_res.stderr or boot_res.stdout).strip()[:400]}"
        )
    _wait_wineserver(wine_info.binary, env)

    reg_content = build_zethropol_registry(tpl.name, tpl.locale)
    reg_file = pfx / "zethropol-init.reg"
    reg_file.write_text(reg_content, encoding="utf-8")
    os.chmod(reg_file, 0o600)

    reg_res = subprocess.run(
        [wine_info.binary, "regedit", "/S", str(reg_file)],
        env=env,
        capture_output=True,
        text=True,
        timeout=60,
        check=False,
    )
    if reg_res.returncode != 0:
        raise CompatibilityError(
            f"wine regedit failed (rc={reg_res.returncode}): {(reg_res.stderr or reg_res.stdout).strip()[:400]}"
        )
    _wait_wineserver(wine_info.binary, env)

    enforce_prefix_isolation(pfx)

    meta = ProfileMetadata(
        schema_version=SCHEMA_VERSION,
        profile=clean_profile,
        template=tpl.name,
        requested_arch=tpl.requested_arch,
        effective_arch=effective_arch,
        windows_version=tpl.windows_version,
        renderer=eff_renderer,
        locale=tpl.locale,
        sandbox=eff_sandbox,
        wine_binary=wine_info.binary,
        wine_version=wine_info.version,
        created_at=utc_now_iso(),
    )
    write_profile_metadata(pfx, meta)

    audit = audit_prefix(clean_profile)
    return (meta, audit)


def audit_prefix(profile: str) -> PrefixAuditReport:
    clean_profile = validate_profile_name(profile)
    pfx = prefix_dir(clean_profile)
    checks: list[AuditCheck] = []

    if not pfx.is_dir():
        checks.append(AuditCheck("prefix_exists", False, f"Prefix directory missing: {pfx}"))
        return PrefixAuditReport(clean_profile, str(pfx), False, checks)
    checks.append(AuditCheck("prefix_exists", True, f"Prefix directory exists: {pfx}"))

    try:
        meta = load_profile_metadata(pfx)
        checks.append(
            AuditCheck(
                "metadata_valid",
                True,
                f"Template={meta.template}, arch={meta.effective_arch}, renderer={meta.renderer}, locale={meta.locale}",
            )
        )
    except CompatibilityError as exc:
        checks.append(AuditCheck("metadata_valid", False, str(exc)))

    dosdevices = pfx / "dosdevices"
    z_drive = dosdevices / "z:"
    escaped_drives: list[str] = []
    if dosdevices.is_dir():
        for entry in sorted(dosdevices.iterdir()):
            if entry.is_symlink():
                target = (entry.parent / os.readlink(entry)).resolve()
                if entry.name.lower() == "z:" or target == Path("/") or not _is_within(target, pfx):
                    escaped_drives.append(f"{entry.name}->{target}")
    if z_drive.exists() or z_drive.is_symlink() or escaped_drives:
        checks.append(
            AuditCheck(
                "z_drive_isolated",
                False,
                f"Unisolated drive mappings detected: {', '.join(escaped_drives) or 'z:'}",
            )
        )
    else:
        checks.append(AuditCheck("z_drive_isolated", True, "Z:\\ root mapping absent; no drive escapes prefix."))

    escaped_user_links: list[str] = []
    users_dir = pfx / "drive_c" / "users"
    if users_dir.is_dir():
        for user_entry in sorted(users_dir.iterdir()):
            if not user_entry.is_dir() or user_entry.is_symlink():
                continue
            for folder in USER_HOME_LINK_DIRS:
                cand = user_entry / folder
                if cand.is_symlink():
                    target = (cand.parent / os.readlink(cand)).resolve()
                    if not _is_within(target, pfx):
                        escaped_user_links.append(str(cand.relative_to(pfx)))
    if escaped_user_links:
        checks.append(
            AuditCheck(
                "home_folders_isolated",
                False,
                f"User folder symlinks escape prefix: {', '.join(escaped_user_links)}",
            )
        )
    else:
        checks.append(AuditCheck("home_folders_isolated", True, "No drive_c/users symlinks point outside prefix."))

    user_reg = pfx / "user.reg"
    sys_reg = pfx / "system.reg"
    user_text = user_reg.read_text(encoding="utf-8", errors="replace") if user_reg.is_file() else ""
    sys_text = sys_reg.read_text(encoding="utf-8", errors="replace") if sys_reg.is_file() else ""

    has_tr_locale = ("0000041F" in user_text) or ("tr-TR" in user_text)
    checks.append(
        AuditCheck(
            "turkish_locale_registry",
            has_tr_locale,
            "Turkish locale (0000041F / tr-TR) present in user.reg."
            if has_tr_locale
            else "Turkish locale entry missing from user.reg.",
        )
    )

    has_fonts = ("Liberation Sans" in sys_text) or ("Liberation Sans" in user_text)
    checks.append(
        AuditCheck(
            "font_substitutes_registry",
            has_fonts,
            "Turkish-capable Liberation/DejaVu FontSubstitutes present in registry."
            if has_fonts
            else "FontSubstitutes entries missing from registry.",
        )
    )

    has_menubuilder_disabled = '"winemenubuilder.exe"=""' in user_text
    checks.append(
        AuditCheck(
            "winemenubuilder_disabled",
            has_menubuilder_disabled,
            "Wine native winemenubuilder.exe is disabled (controlled .lnk converter active)."
            if has_menubuilder_disabled
            else "winemenubuilder.exe override missing from user.reg.",
        )
    )

    overall = all(c.passed for c in checks)
    return PrefixAuditReport(clean_profile, str(pfx), overall, checks)


def list_profiles() -> list[dict[str, Any]]:
    root = prefixes_root()
    if not root.is_dir():
        return []
    result: list[dict[str, Any]] = []
    for entry in sorted(root.iterdir()):
        if not entry.is_dir() or entry.is_symlink():
            continue
        try:
            meta = load_profile_metadata(entry)
            result.append(meta.to_dict())
        except CompatibilityError:
            result.append(
                {
                    "profile": entry.name,
                    "prefix_path": str(entry),
                    "metadata_valid": False,
                }
            )
    return result


def remove_profile(profile: str, confirm: bool = False) -> dict[str, Any]:
    if not confirm:
        raise CompatibilityError("Refusing to remove profile without explicit --yes confirmation.")
    clean_profile = validate_profile_name(profile)
    pfx = prefix_dir(clean_profile)
    if not pfx.exists():
        raise CompatibilityError(f"Profile '{clean_profile}' does not exist at {pfx}.")

    # Clean up only Zethropol-managed desktop entries belonging to this profile
    removed_desktops: list[str] = []
    apps = applications_dir()
    prefix_tag = f"zethropol-wine-{clean_profile}-"
    if apps.is_dir():
        for desktop_file in sorted(apps.glob(f"{prefix_tag}*.desktop")):
            try:
                content = desktop_file.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if "X-Zethropol-Managed=true" in content and f"X-Zethropol-Profile={clean_profile}" in content:
                desktop_file.unlink()
                removed_desktops.append(str(desktop_file))

    shutil.rmtree(pfx)
    return {
        "removed_profile": clean_profile,
        "removed_prefix_path": str(pfx),
        "removed_desktop_files": removed_desktops,
    }
