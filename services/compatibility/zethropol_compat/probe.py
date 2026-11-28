"""Hardware/runtime probing for Wine (WoW64 vs multiarch), Vulkan, DXVK/VKD3D, and bubblewrap."""

from __future__ import annotations

from dataclasses import asdict, dataclass
import os
from pathlib import Path
import re
import shutil
import subprocess
from typing import Any

from .config import CompatibilityError, validate_renderer

SOFTWARE_GPU_MARKERS = ("llvmpipe", "lavapipe", "swiftshader", "softpipe")
HARDWARE_DEVICE_TYPES = (
    "PHYSICAL_DEVICE_TYPE_DISCRETE_GPU",
    "PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU",
    "PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU",
)
DXVK_SYSTEM_DIRS = (
    Path("/usr/share/dxvk/x64"),
    Path("/usr/lib/dxvk/x64"),
    Path("/usr/share/dxvk/x32"),
)
VKD3D_SYSTEM_DIRS = (
    Path("/usr/share/vkd3d-proton/x64"),
    Path("/usr/lib/vkd3d-proton/x64"),
)


@dataclass(frozen=True)
class WineRuntimeInfo:
    binary: str
    version: str
    supports_native_win32: bool
    supports_wow64: bool
    mode: str  # "wow64-only" | "multiarch" | "unknown"

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class VulkanDeviceInfo:
    name: str
    device_type: str
    api_version: str
    driver_name: str
    is_hardware: bool
    meets_dxvk_min_version: bool
    driver_info: str = ""
    vendor_id: str = ""
    device_id: str = ""

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class VulkanProbeResult:
    available: bool
    hardware_vulkan_13: bool
    devices: list[VulkanDeviceInfo]
    reason: str

    def to_dict(self) -> dict[str, Any]:
        return {
            "available": self.available,
            "hardware_vulkan_13": self.hardware_vulkan_13,
            "devices": [d.to_dict() for d in self.devices],
            "reason": self.reason,
        }


@dataclass(frozen=True)
class TranslationLayerProbe:
    dxvk_available: bool
    dxvk_source: str
    vkd3d_available: bool
    vkd3d_source: str

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(frozen=True)
class RendererDecision:
    requested: str
    effective: str  # "dxvk" | "wined3d"
    fallback_applied: bool
    dll_overrides: str
    reason: str
    vkd3d_enabled: bool

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def parse_semver_pair(version_str: str) -> tuple[int, int]:
    m = re.search(r"(\d+)\.(\d+)", version_str)
    if not m:
        return (0, 0)
    return (int(m.group(1)), int(m.group(2)))


def probe_wine(wine_bin: str | None = None) -> WineRuntimeInfo:
    raw = wine_bin or os.environ.get("ZETHROPOL_WINE_BIN", "").strip() or shutil.which("wine") or ""
    if not raw:
        raise CompatibilityError("Wine executable not found in PATH (set ZETHROPOL_WINE_BIN or install 'wine').")

    resolved = Path(raw).resolve()
    if not resolved.is_file():
        raise CompatibilityError(f"Wine executable does not exist: {raw}")

    version = "unknown"
    try:
        res = subprocess.run(
            [str(resolved), "--version"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
        if res.returncode == 0 and res.stdout.strip():
            version = res.stdout.strip().splitlines()[0]
    except OSError:
        pass

    # Detect pure WoW64 (Wine 11 on Arch/CachyOS: x86_64-unix + i386-windows + x86_64-windows, no i386-unix)
    # vs traditional multiarch Wine (has i386-unix).
    prefix_root = resolved.parent.parent
    lib_candidates = (
        prefix_root / "lib" / "wine",
        prefix_root / "lib64" / "wine",
        Path("/usr/lib/wine"),
    )
    has_i386_unix = (prefix_root / "lib32" / "wine").is_dir() or any(
        (c / "i386-unix").is_dir() for c in lib_candidates
    )
    has_x86_64_unix = any((c / "x86_64-unix").is_dir() for c in lib_candidates)
    has_i386_windows = any((c / "i386-windows").is_dir() for c in lib_candidates)
    has_x86_64_windows = any((c / "x86_64-windows").is_dir() for c in lib_candidates)

    supports_wow64 = has_x86_64_unix and has_i386_windows and has_x86_64_windows
    if has_i386_unix:
        mode = "multiarch"
    elif supports_wow64:
        mode = "wow64-only"
    else:
        mode = "unknown"

    return WineRuntimeInfo(
        binary=str(resolved),
        version=version,
        supports_native_win32=has_i386_unix,
        supports_wow64=supports_wow64,
        mode=mode,
    )


def parse_vulkaninfo_summary(summary_text: str) -> VulkanProbeResult:
    devices: list[VulkanDeviceInfo] = []
    in_devices = False
    current: dict[str, str] | None = None

    def flush() -> None:
        if not current:
            return
        name = current.get("deviceName", "").strip()
        dev_type = current.get("deviceType", "").strip()
        api_ver = current.get("apiVersion", "").strip()
        driver_name = current.get("driverName", "").strip()
        driver_info = current.get("driverInfo", "").strip()
        vendor_id = current.get("vendorID", "").strip()
        device_id = current.get("deviceID", "").strip()
        lower_combined = f"{name} {driver_name}".lower()
        is_software = (
            dev_type == "PHYSICAL_DEVICE_TYPE_CPU"
            or any(tok in lower_combined for tok in SOFTWARE_GPU_MARKERS)
        )
        is_hardware = (not is_software) and (dev_type in HARDWARE_DEVICE_TYPES)
        major, minor = parse_semver_pair(api_ver)
        meets_min = (major, minor) >= (1, 3)
        devices.append(
            VulkanDeviceInfo(
                name=name or "unknown",
                device_type=dev_type or "unknown",
                api_version=api_ver or "0.0.0",
                driver_name=driver_name or "unknown",
                is_hardware=is_hardware,
                meets_dxvk_min_version=meets_min,
                driver_info=driver_info,
                vendor_id=vendor_id,
                device_id=device_id,
            )
        )

    for raw_line in summary_text.splitlines():
        line = raw_line.strip()
        if line == "Devices:":
            in_devices = True
            continue
        if not in_devices:
            continue
        if re.match(r"^GPU\d+:$", line):
            flush()
            current = {}
            continue
        if current is not None and "=" in line and not line.startswith("="):
            k, v = line.split("=", 1)
            k = k.strip()
            if k:
                current[k] = v.strip()

    flush()

    if not devices:
        return VulkanProbeResult(
            available=False,
            hardware_vulkan_13=False,
            devices=[],
            reason="No Vulkan GPU devices detected in vulkaninfo summary.",
        )

    hw_13 = [d for d in devices if d.is_hardware and d.meets_dxvk_min_version]
    if hw_13:
        best = hw_13[0]
        return VulkanProbeResult(
            available=True,
            hardware_vulkan_13=True,
            devices=devices,
            reason=f"Hardware Vulkan {best.api_version} GPU detected: {best.name} ({best.driver_name}).",
        )

    hw_any = [d for d in devices if d.is_hardware]
    if hw_any:
        d = hw_any[0]
        return VulkanProbeResult(
            available=True,
            hardware_vulkan_13=False,
            devices=devices,
            reason=f"Hardware GPU '{d.name}' exposes Vulkan {d.api_version} (< 1.3 required by DXVK).",
        )

    return VulkanProbeResult(
        available=True,
        hardware_vulkan_13=False,
        devices=devices,
        reason="Only software Vulkan device detected (llvmpipe/CPU); hardware Vulkan unavailable.",
    )


def probe_vulkan(vulkaninfo_bin: str | None = None) -> VulkanProbeResult:
    binary = vulkaninfo_bin or shutil.which("vulkaninfo")
    if not binary:
        return VulkanProbeResult(
            available=False,
            hardware_vulkan_13=False,
            devices=[],
            reason="vulkaninfo executable not installed; cannot prove hardware Vulkan 1.3+ support.",
        )
    try:
        res = subprocess.run(
            [binary, "--summary"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return VulkanProbeResult(
            available=False,
            hardware_vulkan_13=False,
            devices=[],
            reason=f"vulkaninfo probe failed: {exc}",
        )

    if res.returncode != 0 and "Devices:" not in res.stdout:
        return VulkanProbeResult(
            available=False,
            hardware_vulkan_13=False,
            devices=[],
            reason=f"vulkaninfo exited with code {res.returncode}.",
        )
    return parse_vulkaninfo_summary(res.stdout)


def _is_dxvk_dll(dll_path: Path) -> bool:
    try:
        if not dll_path.is_file() or dll_path.stat().st_size < 16384:
            return False
        data = dll_path.read_bytes()
        return b"DXVK" in data or b"dxvk" in data
    except OSError:
        return False


def probe_translation_layers(
    prefix_path: Path | None = None,
    dxvk_dirs: tuple[Path, ...] = DXVK_SYSTEM_DIRS,
    vkd3d_dirs: tuple[Path, ...] = VKD3D_SYSTEM_DIRS,
) -> TranslationLayerProbe:
    dxvk_available = False
    dxvk_source = "none"
    if prefix_path is not None:
        pfx_d3d11 = prefix_path / "drive_c" / "windows" / "system32" / "d3d11.dll"
        if _is_dxvk_dll(pfx_d3d11):
            dxvk_available = True
            dxvk_source = f"prefix:{pfx_d3d11}"
    if not dxvk_available:
        for d in dxvk_dirs:
            if (d / "d3d11.dll").is_file():
                dxvk_available = True
                dxvk_source = f"system:{d}"
                break

    vkd3d_available = False
    vkd3d_source = "none"
    if prefix_path is not None:
        pfx_d3d12 = prefix_path / "drive_c" / "windows" / "system32" / "d3d12core.dll"
        if pfx_d3d12.is_file():
            vkd3d_available = True
            vkd3d_source = f"prefix:{pfx_d3d12}"
    if not vkd3d_available:
        for d in vkd3d_dirs:
            if (d / "d3d12.dll").is_file() or (d / "d3d12core.dll").is_file():
                vkd3d_available = True
                vkd3d_source = f"system:{d}"
                break

    return TranslationLayerProbe(
        dxvk_available=dxvk_available,
        dxvk_source=dxvk_source,
        vkd3d_available=vkd3d_available,
        vkd3d_source=vkd3d_source,
    )


def select_renderer(
    requested: str,
    vulkan: VulkanProbeResult,
    layers: TranslationLayerProbe,
) -> RendererDecision:
    req = validate_renderer(requested)
    if req == "wined3d":
        return RendererDecision(
            requested=req,
            effective="wined3d",
            fallback_applied=False,
            dll_overrides="d3d9,d3d10core,d3d11,dxgi=b",
            reason="Explicitly configured to use WineD3D (OpenGL/legacy fallback).",
            vkd3d_enabled=False,
        )

    can_use_dxvk = vulkan.hardware_vulkan_13 and layers.dxvk_available
    if can_use_dxvk:
        overrides = "d3d9,d3d10core,d3d11,dxgi=n,b"
        if layers.vkd3d_available:
            overrides += ";d3d12,d3d12core=n,b"
        return RendererDecision(
            requested=req,
            effective="dxvk",
            fallback_applied=False,
            dll_overrides=overrides,
            reason=f"Hardware Vulkan 1.3+ and DXVK ({layers.dxvk_source}) verified.",
            vkd3d_enabled=layers.vkd3d_available,
        )

    reasons: list[str] = []
    if not vulkan.hardware_vulkan_13:
        reasons.append(vulkan.reason)
    if not layers.dxvk_available:
        reasons.append("DXVK DLLs not installed (install optional 'dxvk-mingw-git' to enable DXVK).")
    detail = " ".join(reasons)

    if req == "dxvk":
        raise CompatibilityError(f"Requested renderer 'dxvk' is unavailable: {detail}")

    return RendererDecision(
        requested=req,
        effective="wined3d",
        fallback_applied=True,
        dll_overrides="d3d9,d3d10core,d3d11,dxgi=b",
        reason=f"Falling back to WineD3D: {detail}",
        vkd3d_enabled=False,
    )


def probe_bwrap() -> str | None:
    return shutil.which("bwrap")
