"""Isolated .exe/.msi/.lnk runner with automatic Vulkan/DXVK vs WineD3D selection and bwrap sandboxing."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

from .config import (
    CompatibilityError,
    ProfileMetadata,
    SUPPORTED_TARGET_EXTENSIONS,
    load_profile_metadata,
    prefix_dir,
    validate_profile_name,
    validate_renderer,
)
from .prefix import enforce_prefix_isolation, init_prefix, resolve_effective_arch
from .probe import (
    RendererDecision,
    TranslationLayerProbe,
    VulkanProbeResult,
    WineRuntimeInfo,
    probe_bwrap,
    probe_translation_layers,
    probe_vulkan,
    probe_wine,
    select_renderer,
)
from .shortcut import parse_lnk, resolve_lnk_target_in_prefix, sync_prefix_shortcuts

SANDBOX_TARGET_MOUNT = "/run/zethropol-target"


@dataclass(frozen=True)
class LaunchPlan:
    profile: str
    prefix_path: str
    target_host_path: str
    target_windows_path: str
    target_kind: str  # "exe" | "msi"
    effective_arch: str
    wine_binary: str
    wine_version: str
    renderer: RendererDecision
    vulkan: VulkanProbeResult
    translation_layers: TranslationLayerProbe
    sandbox_enabled: bool
    sandbox_tool: str
    sandbox_warning: str
    transient_drive_target: str | None
    command: list[str]
    env_overrides: dict[str, str]

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile": self.profile,
            "prefix_path": self.prefix_path,
            "target_host_path": self.target_host_path,
            "target_windows_path": self.target_windows_path,
            "target_kind": self.target_kind,
            "effective_arch": self.effective_arch,
            "wine_binary": self.wine_binary,
            "wine_version": self.wine_version,
            "renderer": self.renderer.to_dict(),
            "vulkan": self.vulkan.to_dict(),
            "translation_layers": self.translation_layers.to_dict(),
            "sandbox_enabled": self.sandbox_enabled,
            "sandbox_tool": self.sandbox_tool,
            "sandbox_warning": self.sandbox_warning,
            "transient_drive_target": self.transient_drive_target,
            "command": self.command,
            "env_overrides": self.env_overrides,
        }


def _to_drive_c_windows_path(target_resolved: Path, prefix_resolved: Path) -> str | None:
    drive_c = (prefix_resolved / "drive_c").resolve()
    try:
        rel = target_resolved.relative_to(drive_c)
    except ValueError:
        return None
    return "C:\\" + "\\".join(rel.parts)


def _build_bwrap_command(
    bwrap_bin: str,
    prefix_resolved: Path,
    external_parent: Path | None,
    inner_cmd: list[str],
    env: dict[str, str],
) -> list[str]:
    home_dir = str(Path.home().resolve())
    cmd: list[str] = [
        bwrap_bin,
        "--unshare-pid",
        "--die-with-parent",
        "--new-session",
        "--ro-bind", "/usr", "/usr",
        "--ro-bind", "/etc", "/etc",
        "--ro-bind-try", "/bin", "/bin",
        "--ro-bind-try", "/sbin", "/sbin",
        "--ro-bind-try", "/lib", "/lib",
        "--ro-bind-try", "/lib64", "/lib64",
        "--ro-bind-try", "/opt", "/opt",
        "--ro-bind", "/sys", "/sys",
        "--proc", "/proc",
        "--dev", "/dev",
        "--dev-bind-try", "/dev/dri", "/dev/dri",
        "--dev-bind-try", "/dev/ntsync", "/dev/ntsync",
        "--bind-try", "/dev/shm", "/dev/shm",
        "--tmpfs", "/tmp",
        "--tmpfs", "/run",
        "--tmpfs", home_dir,
        "--bind", str(prefix_resolved), str(prefix_resolved),
    ]

    # Bind only the external target's parent directory read-only when launching outside drive_c
    if external_parent is not None:
        cmd.extend(["--ro-bind", str(external_parent), SANDBOX_TARGET_MOUNT])

    # Expose X11 / Wayland / PipeWire / Pulse runtime sockets safely if present
    x11_dir = Path("/tmp/.X11-unix")
    if env.get("DISPLAY") and x11_dir.is_dir():
        cmd.extend(["--ro-bind", str(x11_dir), str(x11_dir)])
    xauth = env.get("XAUTHORITY") or str(Path.home() / ".Xauthority")
    if env.get("DISPLAY") and Path(xauth).is_file():
        cmd.extend(["--ro-bind", xauth, xauth])

    xdg_rt = env.get("XDG_RUNTIME_DIR", "").strip()
    if xdg_rt and Path(xdg_rt).is_dir():
        cmd.extend(["--dir", xdg_rt])
        for sock_name in (
            env.get("WAYLAND_DISPLAY", ""),
            "pipewire-0",
            "pulse",
        ):
            if not sock_name or "/" in sock_name or ".." in sock_name:
                continue
            sock_path = Path(xdg_rt) / sock_name
            if sock_path.exists():
                cmd.extend(["--bind", str(sock_path), str(sock_path)])

    cmd.append("--")
    cmd.extend(inner_cmd)
    return cmd


def build_launch_plan(
    target_path: Path,
    extra_args: list[str] | None = None,
    profile: str = "default-win64",
    renderer_override: str | None = None,
    sandbox_override: bool | None = None,
    wine_bin: str | None = None,
    auto_init_default: bool = True,
    vulkan_probe: VulkanProbeResult | None = None,
    layer_probe: TranslationLayerProbe | None = None,
    bwrap_bin: str | None = None,
    wine_info: WineRuntimeInfo | None = None,
) -> LaunchPlan:
    clean_profile = validate_profile_name(profile)
    pfx = prefix_dir(clean_profile)

    if not pfx.is_dir():
        if auto_init_default and clean_profile in ("default-win64", "classic-win32"):
            init_prefix(clean_profile, template=clean_profile, wine_bin=wine_bin)
        else:
            raise CompatibilityError(
                f"Profile '{clean_profile}' is not initialized. Run: zethropol-prefix-ctl init {clean_profile}"
            )

    meta: ProfileMetadata = load_profile_metadata(pfx)
    target_expanded = target_path.expanduser()
    if not target_expanded.is_file():
        raise CompatibilityError(f"Target file not found: {target_path}")
    target_resolved = target_expanded.resolve()

    suffix = target_resolved.suffix.lower()
    if suffix not in SUPPORTED_TARGET_EXTENSIONS:
        raise CompatibilityError(
            f"Unsupported target file type '{suffix}': expected one of {', '.join(SUPPORTED_TARGET_EXTENSIONS)}"
        )

    args = list(extra_args or [])
    if suffix == ".lnk":
        lnk = parse_lnk(target_resolved)
        target_resolved = resolve_lnk_target_in_prefix(lnk, pfx)
        suffix = target_resolved.suffix.lower()
        if lnk.arguments and not args:
            args = lnk.arguments.split()

    target_kind = "msi" if suffix == ".msi" else "exe"
    effective_wine = wine_info or probe_wine(wine_bin or meta.wine_binary)
    winearch_env, effective_arch = resolve_effective_arch(meta.requested_arch, effective_wine)

    vk = vulkan_probe if vulkan_probe is not None else probe_vulkan()
    layers = layer_probe if layer_probe is not None else probe_translation_layers(pfx)
    req_renderer = validate_renderer(renderer_override) if renderer_override is not None else meta.renderer
    decision = select_renderer(req_renderer, vk, layers)

    want_sandbox = meta.sandbox if sandbox_override is None else bool(sandbox_override)
    detected_bwrap = bwrap_bin if bwrap_bin is not None else probe_bwrap()
    sandbox_warning = ""
    if want_sandbox and detected_bwrap:
        sandbox_enabled = True
        sandbox_tool = detected_bwrap
    elif want_sandbox and not detected_bwrap:
        sandbox_enabled = False
        sandbox_tool = "none"
        sandbox_warning = (
            "bubblewrap (bwrap) not found; running with prefix-only Z:\\ isolation (install 'bubblewrap' for OS-level write sandboxing)."
        )
    else:
        sandbox_enabled = False
        sandbox_tool = "disabled"
        sandbox_warning = "OS-level bwrap sandbox explicitly disabled (--no-sandbox); prefix Z:\\ isolation remains active."

    # Resolve Windows path (prefer native C:\ if already inside prefix drive_c; else scoped read-only Y:\)
    in_drive_c = _to_drive_c_windows_path(target_resolved, pfx)
    if in_drive_c is not None:
        win_target = in_drive_c
        external_parent: Path | None = None
        transient_drive_target: str | None = None
    else:
        win_target = f"Y:\\{target_resolved.name}"
        external_parent = target_resolved.parent
        transient_drive_target = SANDBOX_TARGET_MOUNT if sandbox_enabled else str(external_parent)

    if target_kind == "msi":
        inner_cmd = [effective_wine.binary, "msiexec", "/i", win_target, *args]
    else:
        inner_cmd = [effective_wine.binary, win_target, *args]

    dll_overrides = f"{decision.dll_overrides};winemenubuilder.exe="
    env_overrides: dict[str, str] = {
        "WINEPREFIX": str(pfx),
        "WINEARCH": winearch_env,
        "LANG": meta.locale,
        "LC_ALL": meta.locale,
        "LC_CTYPE": meta.locale,
        "WINEDLLOVERRIDES": dll_overrides,
        "WINEDEBUG": "-all",
    }
    if decision.effective == "dxvk":
        env_overrides["DXVK_LOG_LEVEL"] = "warn"
    else:
        env_overrides["PROTON_USE_WINED3D"] = "1"

    merged_env = os.environ.copy()
    merged_env.update(env_overrides)

    if sandbox_enabled:
        final_cmd = _build_bwrap_command(
            detected_bwrap,
            pfx,
            external_parent,
            inner_cmd,
            merged_env,
        )
    else:
        final_cmd = inner_cmd

    return LaunchPlan(
        profile=clean_profile,
        prefix_path=str(pfx),
        target_host_path=str(target_resolved),
        target_windows_path=win_target,
        target_kind=target_kind,
        effective_arch=effective_arch,
        wine_binary=effective_wine.binary,
        wine_version=effective_wine.version,
        renderer=decision,
        vulkan=vk,
        translation_layers=layers,
        sandbox_enabled=sandbox_enabled,
        sandbox_tool=sandbox_tool,
        sandbox_warning=sandbox_warning,
        transient_drive_target=transient_drive_target,
        command=final_cmd,
        env_overrides=env_overrides,
    )


def execute_launch_plan(
    plan: LaunchPlan,
    sync_shortcuts: bool = True,
    runner_bin: str = "/usr/bin/zethropol-wine-run",
) -> int:
    pfx = Path(plan.prefix_path)
    enforce_prefix_isolation(pfx)

    if plan.sandbox_warning:
        print(f"[zethropol-wine-run] WARNING: {plan.sandbox_warning}", file=sys.stderr)

    y_link = pfx / "dosdevices" / "y:"
    if y_link.exists() or y_link.is_symlink():
        y_link.unlink()

    env = os.environ.copy()
    env.update(plan.env_overrides)

    try:
        if plan.transient_drive_target is not None:
            (pfx / "dosdevices").mkdir(parents=True, exist_ok=True)
            y_link.symlink_to(plan.transient_drive_target)
        res = subprocess.run(plan.command, env=env, check=False)
        rc = res.returncode
    finally:
        if y_link.exists() or y_link.is_symlink():
            y_link.unlink()
        enforce_prefix_isolation(pfx)

    if sync_shortcuts:
        try:
            sync_prefix_shortcuts(plan.profile, runner_bin=runner_bin)
        except CompatibilityError:
            pass

    return rc
