"""Stage 07 Gaming readiness, GPU capability matrix (Polaris11/Broadwell/Vega3), and isolated prefix DXVK/VKD3D management."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
from typing import Any

from .config import (
    CompatibilityError,
    load_profile_metadata,
    prefix_dir,
    utc_now_iso,
    validate_profile_name,
    xdg_data_home,
)
from .prefix import enforce_prefix_isolation
from .probe import (
    DXVK_SYSTEM_DIRS,
    VKD3D_SYSTEM_DIRS,
    VulkanProbeResult,
    probe_bwrap,
    probe_translation_layers,
    probe_vulkan,
    probe_wine,
)

ANTICHEAT_EULA_NOTICE = (
    "Uyarı: Çekirdek seviyesi anti-cheat atlatma, ikili/bellek müdahalesi veya oyun EULA / kullanım koşulları "
    "ihlali kesinlikle yasaktır. Yalnızca Linux/Proton üzerinde resmi olarak desteklenen oyun ve anti-cheat "
    "çalışma zamanları kullanılmalıdır."
)

GAMING_LAYERS_MANIFEST = "zethropol-gaming-layers.json"

DXVK_DLL_NAMES = ("d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll")
VKD3D_DLL_NAMES = ("d3d12.dll", "d3d12core.dll")

DXVK_X64_DIRS = (Path("/usr/share/dxvk/x64"), Path("/usr/lib/dxvk/x64"))
DXVK_X32_DIRS = (Path("/usr/share/dxvk/x32"), Path("/usr/lib/dxvk/x32"))
VKD3D_X64_DIRS = (Path("/usr/share/vkd3d-proton/x64"), Path("/usr/lib/vkd3d-proton/x64"))
VKD3D_X86_DIRS = (Path("/usr/share/vkd3d-proton/x86"), Path("/usr/lib/vkd3d-proton/x86"))


def anticheat_policy_dict() -> dict[str, Any]:
    return {
        "kernel_anticheat_bypass_forbidden": True,
        "license_eula_compliance_required": True,
        "notice": ANTICHEAT_EULA_NOTICE,
    }


def read_primary_vram_total_gb(drm_dir: Path = Path("/sys/class/drm")) -> float | None:
    if not drm_dir.is_dir():
        return None
    cards = sorted(
        (p for p in drm_dir.iterdir() if p.name.startswith("card") and p.name[4:].isdigit()),
        key=lambda p: int(p.name[4:]),
    )
    for card in cards:
        vram_file = card / "device" / "mem_info_vram_total"
        try:
            raw = vram_file.read_text(encoding="utf-8").strip()
            total_bytes = float(raw)
            if total_bytes > 0:
                return round(total_bytes / 1_073_741_824.0, 2)
        except (OSError, ValueError):
            continue
    return None


def classify_gpu_capabilities(
    vulkan: VulkanProbeResult,
    vram_total_gb: float | None = None,
) -> dict[str, Any]:
    primary = next((d for d in vulkan.devices if d.is_hardware and d.meets_dxvk_min_version), None)
    if primary is None:
        primary = next((d for d in vulkan.devices if d.is_hardware), None)
    if primary is None and vulkan.devices:
        primary = vulkan.devices[0]

    if primary is None:
        return {
            "matched_profile": "unavailable",
            "tier": "vulkan_unavailable",
            "tier_label": "Vulkan Kullanılamıyor",
            "gpu_name": "Algılanmadı",
            "driver_name": "none",
            "driver_info": "",
            "vulkan_api_version": "N/A",
            "hardware_vulkan_13": False,
            "dxvk_supported": False,
            "vkd3d_supported": False,
            "vram_total_gb": vram_total_gb,
            "limitations": [vulkan.reason],
            "recommendations": [
                "Vulkan sürücüsünü ve 'vulkan-tools' paketini doğrulayın; o zamana kadar WineD3D yedek motoru kullanılır."
            ],
        }

    combined = " ".join(
        [primary.name, primary.driver_name, primary.driver_info, primary.vendor_id, primary.device_id]
    ).lower()

    if not primary.is_hardware:
        return {
            "matched_profile": "software-vulkan",
            "tier": "software_only",
            "tier_label": "Yalnızca Yazılım Vulkan (CPU)",
            "gpu_name": primary.name,
            "driver_name": primary.driver_name,
            "driver_info": primary.driver_info,
            "vulkan_api_version": primary.api_version,
            "hardware_vulkan_13": False,
            "dxvk_supported": False,
            "vkd3d_supported": False,
            "vram_total_gb": vram_total_gb,
            "limitations": ["Donanım GPU hızlandırması algılanmadı (llvmpipe/CPU yazılım yürütmesi)."],
            "recommendations": ["Uygun donanım Vulkan sürücüsü yoksa WineD3D kullanın."],
        }

    if any(tok in combined for tok in ("polaris11", "rx 560", "0x67ef", "67ef")):
        limitations = [
            "AMD Polaris11 (GCN 4 / RADV): Donanım ışın izleme ve mesh-shader desteği yoktur; VKD3D-Proton (D3D12) yalnızca sınırlı/hafif başlıklarda uygundur.",
            "RX 560/560D termal sınırı (~82 °C) ve 4 GB sınıfı VRAM bütçesi nedeniyle ağır doku paketlerinde VRAM taşması ve takılma oluşabilir.",
        ]
        if vram_total_gb is not None and vram_total_gb <= 4.2:
            limitations.append(
                f"Ölçülen VRAM ({vram_total_gb:.2f} GB): Modern D3D12 başlıklarında düşük/orta doku ayarları önerilir."
            )
        return {
            "matched_profile": "amd-rx560-polaris11",
            "tier": "dxvk_ready_vkd3d_limited",
            "tier_label": "DXVK Hazır · VKD3D Sınırlı (Polaris11)",
            "gpu_name": primary.name,
            "driver_name": primary.driver_name,
            "driver_info": primary.driver_info,
            "vulkan_api_version": primary.api_version,
            "hardware_vulkan_13": primary.meets_dxvk_min_version,
            "dxvk_supported": primary.meets_dxvk_min_version,
            "vkd3d_supported": False,
            "vram_total_gb": vram_total_gb,
            "limitations": limitations,
            "recommendations": [
                "D3D9/10/11 oyunları için RADV + DXVK kullanın.",
                "GPU sıcaklığını 82 °C altında tutarak termal kısıtlamadan kaçının.",
            ],
        }

    if any(tok in combined for tok in ("hd graphics 5500", "broadwell", "bdw", "hasvk", "0x1616")):
        return {
            "matched_profile": "intel-broadwell-hd5500",
            "tier": "legacy_vulkan_12_wined3d",
            "tier_label": "Eski Vulkan 1.2 · WineD3D Önerilir (Broadwell)",
            "gpu_name": primary.name,
            "driver_name": primary.driver_name,
            "driver_info": primary.driver_info,
            "vulkan_api_version": primary.api_version,
            "hardware_vulkan_13": primary.meets_dxvk_min_version,
            "dxvk_supported": False,
            "vkd3d_supported": False,
            "vram_total_gb": vram_total_gb,
            "limitations": [
                f"Intel Broadwell HD 5500 (Mesa intel_hasvk) Vulkan {primary.api_version} ile sınırlıdır; DXVK 2.x (Vulkan 1.3+) ve VKD3D-Proton desteklenmez.",
                "Paylaşımlı sistem belleği ve sınırlı GPU kaynakları nedeniyle yalnızca klasik/hafif 2D ve eski 3D oyunlar uygundur.",
            ],
            "recommendations": [
                "Wine profillerinde WineD3D (OpenGL) veya yerel Linux oyunlarını tercih edin."
            ],
        }

    if any(tok in combined for tok in ("vega 3", "vega3", "raven2", "0x15d8", "0x15dd")):
        return {
            "matched_profile": "amd-vega3-apu",
            "tier": "apu_dxvk_light",
            "tier_label": "Hafif Oyunlar İçin DXVK Uygun (Vega 3 APU)",
            "gpu_name": primary.name,
            "driver_name": primary.driver_name,
            "driver_info": primary.driver_info,
            "vulkan_api_version": primary.api_version,
            "hardware_vulkan_13": primary.meets_dxvk_min_version,
            "dxvk_supported": primary.meets_dxvk_min_version,
            "vkd3d_supported": False,
            "vram_total_gb": vram_total_gb,
            "limitations": [
                "AMD Vega 3 tümleşik APU (3 CU) paylaşımlı sistem RAM/UMA bant genişliği ve düşük termal zarfla sınırlıdır.",
                "VKD3D-Proton (D3D12) ağır modern oyunlarda yetersiz performans ve bellek baskısı oluşturabilir.",
            ],
            "recommendations": [
                "720p/düşük ayarlarda hafif D3D9-11 ve bağımsız (indie) oyunlar için RADV + DXVK kullanın."
            ],
        }

    if primary.meets_dxvk_min_version:
        return {
            "matched_profile": "generic-vulkan-13",
            "tier": "dxvk_vkd3d_ready",
            "tier_label": "Vulkan 1.3+ · DXVK/VKD3D Uygun",
            "gpu_name": primary.name,
            "driver_name": primary.driver_name,
            "driver_info": primary.driver_info,
            "vulkan_api_version": primary.api_version,
            "hardware_vulkan_13": True,
            "dxvk_supported": True,
            "vkd3d_supported": True,
            "vram_total_gb": vram_total_gb,
            "limitations": [],
            "recommendations": ["DXVK ve VKD3D-Proton için donanım Vulkan 1.3+ doğrulandı."],
        }

    return {
        "matched_profile": "generic-legacy-vulkan",
        "tier": "legacy_vulkan_wined3d",
        "tier_label": "Eski Vulkan (< 1.3) · WineD3D Yedek",
        "gpu_name": primary.name,
        "driver_name": primary.driver_name,
        "driver_info": primary.driver_info,
        "vulkan_api_version": primary.api_version,
        "hardware_vulkan_13": False,
        "dxvk_supported": False,
        "vkd3d_supported": False,
        "vram_total_gb": vram_total_gb,
        "limitations": [
            f"GPU '{primary.name}' Vulkan {primary.api_version} sunuyor (< 1.3); DXVK 2.x ve VKD3D-Proton kullanılamaz."
        ],
        "recommendations": ["WineD3D yedek motorunu kullanın."],
    }


def _find_proton_installations(extra_dirs: tuple[Path, ...] = ()) -> tuple[bool, str]:
    candidates: list[Path] = [
        *extra_dirs,
        Path("/usr/share/steam/compatibilitytools.d"),
        xdg_data_home() / "Steam" / "compatibilitytools.d",
        xdg_data_home() / "Steam" / "steamapps" / "common",
        Path.home() / ".steam" / "root" / "compatibilitytools.d",
        Path.home() / ".steam" / "root" / "steamapps" / "common",
    ]
    for base in candidates:
        if not base.is_dir():
            continue
        try:
            for entry in sorted(base.iterdir()):
                if entry.is_dir() and "proton" in entry.name.lower():
                    return (True, str(entry))
        except OSError:
            continue
    return (False, "none")


def probe_gaming_stack(
    prefix_path: Path | None = None,
    vulkan_probe: VulkanProbeResult | None = None,
    dxvk_dirs: tuple[Path, ...] = DXVK_SYSTEM_DIRS,
    vkd3d_dirs: tuple[Path, ...] = VKD3D_SYSTEM_DIRS,
    proton_dirs: tuple[Path, ...] = (),
    vram_total_gb: float | None = None,
) -> dict[str, Any]:
    vk = vulkan_probe if vulkan_probe is not None else probe_vulkan()
    vram = vram_total_gb if vram_total_gb is not None else read_primary_vram_total_gb()
    gpu_caps = classify_gpu_capabilities(vk, vram_total_gb=vram)
    layers = probe_translation_layers(prefix_path=prefix_path, dxvk_dirs=dxvk_dirs, vkd3d_dirs=vkd3d_dirs)

    steam_bin = shutil.which("steam")
    gamescope_bin = shutil.which("gamescope")
    mangohud_bin = shutil.which("mangohud")
    gamemode_bin = shutil.which("gamemoded")
    bwrap_bin = probe_bwrap()
    proton_ok, proton_src = _find_proton_installations(proton_dirs)

    missing: list[str] = []
    if not vk.available:
        missing.append("vulkan-tools / Vulkan sürücüsü")
    if gpu_caps["dxvk_supported"] and not layers.dxvk_available:
        missing.append("dxvk-mingw-git (isteğe bağlı D3D9/10/11 çeviri katmanı)")
    if gpu_caps["vkd3d_supported"] and not layers.vkd3d_available:
        missing.append("vkd3d-proton-mingw-git (isteğe bağlı D3D12 çeviri katmanı)")
    if not steam_bin:
        missing.append("steam (isteğe bağlı oyun istemcisi)")
    if not gamescope_bin:
        missing.append("gamescope (isteğe bağlı mikro-kompozitör)")
    if not mangohud_bin:
        missing.append("mangohud (isteğe bağlı performans katmanı)")
    if not gamemode_bin:
        missing.append("gamemode (isteğe bağlı oturum zamanlayıcı)")

    return {
        "gpu_capabilities": gpu_caps,
        "vulkan": vk.to_dict(),
        "translation_layers": layers.to_dict(),
        "tools": {
            "steam": steam_bin or "not-installed",
            "steam_available": bool(steam_bin),
            "proton_available": proton_ok,
            "proton_source": proton_src,
            "gamescope": gamescope_bin or "not-installed",
            "gamescope_available": bool(gamescope_bin),
            "mangohud": mangohud_bin or "not-installed",
            "mangohud_available": bool(mangohud_bin),
            "gamemode": gamemode_bin or "not-installed",
            "gamemode_available": bool(gamemode_bin),
            "bwrap": bwrap_bin or "not-installed",
            "bwrap_available": bool(bwrap_bin),
        },
        "missing_components": missing,
        "anticheat_eula": anticheat_policy_dict(),
    }


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def _find_regular_dll_dir(candidates: tuple[Path, ...], required_any: tuple[str, ...]) -> Path | None:
    for cand in candidates:
        if not cand.is_dir() or cand.is_symlink():
            continue
        for name in required_any:
            f = cand / name
            if f.is_file() and not f.is_symlink():
                return cand
    return None


def _copy_layer_dlls(
    source_dir: Path,
    dest_dir: Path,
    prefix_resolved: Path,
    dll_names: tuple[str, ...],
) -> list[dict[str, str]]:
    source_resolved = source_dir.resolve()
    dest_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not _is_within(dest_dir, prefix_resolved):
        raise CompatibilityError(f"Destination directory escaped prefix: {dest_dir}")

    copied: list[dict[str, str]] = []
    for dll in dll_names:
        src = source_dir / dll
        if not src.exists():
            continue
        if src.is_symlink() or not src.is_file() or not _is_within(src, source_resolved):
            raise CompatibilityError(f"Refusing non-regular or escaping DLL source: {src}")
        dst = dest_dir / dll
        if dst.is_symlink():
            dst.unlink()
        shutil.copyfile(src, dst)
        os.chmod(dst, 0o644)
        copied.append(
            {
                "dll": dll,
                "relative_path": str(dst.resolve().relative_to(prefix_resolved)),
                "source": str(src),
                "sha256": _sha256_file(dst),
            }
        )
    return copied


def get_prefix_gaming_status(
    profile: str,
    vulkan_probe: VulkanProbeResult | None = None,
    dxvk_x64_dirs: tuple[Path, ...] = DXVK_X64_DIRS,
    vkd3d_x64_dirs: tuple[Path, ...] = VKD3D_X64_DIRS,
) -> dict[str, Any]:
    clean = validate_profile_name(profile)
    pfx = prefix_dir(clean)
    if not pfx.is_dir():
        raise CompatibilityError(f"Profile '{clean}' is not initialized at {pfx}.")
    meta = load_profile_metadata(pfx)

    vk = vulkan_probe if vulkan_probe is not None else probe_vulkan()
    gpu_caps = classify_gpu_capabilities(vk)
    sys32 = pfx / "drive_c" / "windows" / "system32"
    manifest_path = pfx / GAMING_LAYERS_MANIFEST
    manifest: dict[str, Any] = {}
    if manifest_path.is_file():
        try:
            raw = json.loads(manifest_path.read_text(encoding="utf-8"))
            if isinstance(raw, dict):
                manifest = raw
        except (OSError, json.JSONDecodeError):
            manifest = {}

    dxvk_files = [f for f in manifest.get("dxvk_files", []) if isinstance(f, dict)]
    vkd3d_files = [f for f in manifest.get("vkd3d_files", []) if isinstance(f, dict)]

    dxvk_installed = bool(dxvk_files) and all((pfx / item.get("relative_path", "")).is_file() for item in dxvk_files)
    vkd3d_installed = bool(vkd3d_files) and all((pfx / item.get("relative_path", "")).is_file() for item in vkd3d_files)

    # Also detect pre-existing prefix DLLs if manually placed
    if not dxvk_installed and (sys32 / "d3d11.dll").is_file():
        layers = probe_translation_layers(prefix_path=pfx, dxvk_dirs=(), vkd3d_dirs=())
        dxvk_installed = layers.dxvk_available
    if not vkd3d_installed and (sys32 / "d3d12core.dll").is_file():
        vkd3d_installed = True

    system_dxvk = _find_regular_dll_dir(dxvk_x64_dirs, ("d3d11.dll",))
    system_vkd3d = _find_regular_dll_dir(vkd3d_x64_dirs, ("d3d12.dll", "d3d12core.dll"))

    return {
        "profile": clean,
        "prefix_path": str(pfx),
        "renderer": meta.renderer,
        "dxvk_installed": dxvk_installed,
        "vkd3d_installed": vkd3d_installed,
        "managed_manifest_present": manifest_path.is_file(),
        "dxvk_files_count": len(dxvk_files),
        "vkd3d_files_count": len(vkd3d_files),
        "system_dxvk_available": system_dxvk is not None,
        "system_dxvk_source": str(system_dxvk) if system_dxvk else "none",
        "system_vkd3d_available": system_vkd3d is not None,
        "system_vkd3d_source": str(system_vkd3d) if system_vkd3d else "none",
        "hardware_vulkan_13": vk.hardware_vulkan_13,
        "gpu_capabilities": gpu_caps,
        "anticheat_eula": anticheat_policy_dict(),
    }


def install_prefix_gaming_layers(
    profile: str,
    include_dxvk: bool = True,
    include_vkd3d: bool = False,
    accept_notice: bool = False,
    vulkan_probe: VulkanProbeResult | None = None,
    dxvk_x64_dirs: tuple[Path, ...] = DXVK_X64_DIRS,
    dxvk_x32_dirs: tuple[Path, ...] = DXVK_X32_DIRS,
    vkd3d_x64_dirs: tuple[Path, ...] = VKD3D_X64_DIRS,
    vkd3d_x86_dirs: tuple[Path, ...] = VKD3D_X86_DIRS,
) -> dict[str, Any]:
    if not accept_notice:
        raise CompatibilityError(
            "Anti-cheat / EULA kullanım koşulu uyarısı açıkça onaylanmadan (--accept-notice) DXVK/VKD3D kurulamaz."
        )
    if not include_dxvk and not include_vkd3d:
        raise CompatibilityError("En az bir çeviri katmanı seçilmelidir (DXVK veya VKD3D-Proton).")

    clean = validate_profile_name(profile)
    pfx = prefix_dir(clean).resolve()
    if not pfx.is_dir():
        raise CompatibilityError(f"Profile '{clean}' is not initialized at {pfx}.")
    load_profile_metadata(pfx)

    vk = vulkan_probe if vulkan_probe is not None else probe_vulkan()
    if not vk.hardware_vulkan_13:
        raise CompatibilityError(
            f"Donanım Vulkan 1.3+ doğrulanamadı ({vk.reason}); bu GPU için DXVK/VKD3D kurulumu engellendi (WineD3D önerilir)."
        )

    sys32 = pfx / "drive_c" / "windows" / "system32"
    syswow64 = pfx / "drive_c" / "windows" / "syswow64"

    dxvk_copied: list[dict[str, str]] = []
    vkd3d_copied: list[dict[str, str]] = []

    if include_dxvk:
        src_x64 = _find_regular_dll_dir(dxvk_x64_dirs, ("d3d11.dll",))
        if src_x64 is None:
            raise CompatibilityError(
                "Sistemde DXVK x64 DLL dosyaları bulunamadı (isteğe bağlı 'dxvk-mingw-git' paketini kurun)."
            )
        dxvk_copied.extend(_copy_layer_dlls(src_x64, sys32, pfx, DXVK_DLL_NAMES))
        src_x32 = _find_regular_dll_dir(dxvk_x32_dirs, ("d3d11.dll",))
        if src_x32 is not None:
            dxvk_copied.extend(_copy_layer_dlls(src_x32, syswow64, pfx, DXVK_DLL_NAMES))

    if include_vkd3d:
        src_v64 = _find_regular_dll_dir(vkd3d_x64_dirs, ("d3d12.dll", "d3d12core.dll"))
        if src_v64 is None:
            raise CompatibilityError(
                "Sistemde VKD3D-Proton x64 DLL dosyaları bulunamadı (isteğe bağlı 'vkd3d-proton-mingw-git' paketini kurun)."
            )
        vkd3d_copied.extend(_copy_layer_dlls(src_v64, sys32, pfx, VKD3D_DLL_NAMES))
        src_v86 = _find_regular_dll_dir(vkd3d_x86_dirs, ("d3d12.dll", "d3d12core.dll"))
        if src_v86 is not None:
            vkd3d_copied.extend(_copy_layer_dlls(src_v86, syswow64, pfx, VKD3D_DLL_NAMES))

    manifest_path = pfx / GAMING_LAYERS_MANIFEST
    manifest_payload = {
        "schema_version": 1,
        "profile": clean,
        "installed_at": utc_now_iso(),
        "eula_notice_accepted": True,
        "dxvk_files": dxvk_copied,
        "vkd3d_files": vkd3d_copied,
    }
    tmp = pfx / f".{GAMING_LAYERS_MANIFEST}.tmp"
    tmp.write_text(json.dumps(manifest_payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    os.chmod(tmp, 0o600)
    tmp.replace(manifest_path)

    enforce_prefix_isolation(pfx)

    return {
        "profile": clean,
        "prefix_path": str(pfx),
        "dxvk_installed": bool(dxvk_copied),
        "vkd3d_installed": bool(vkd3d_copied),
        "dxvk_files": dxvk_copied,
        "vkd3d_files": vkd3d_copied,
        "anticheat_eula": anticheat_policy_dict(),
    }


def remove_prefix_gaming_layers(profile: str) -> dict[str, Any]:
    clean = validate_profile_name(profile)
    pfx = prefix_dir(clean).resolve()
    if not pfx.is_dir():
        raise CompatibilityError(f"Profile '{clean}' is not initialized at {pfx}.")
    load_profile_metadata(pfx)

    manifest_path = pfx / GAMING_LAYERS_MANIFEST
    removed: list[str] = []
    if manifest_path.is_file():
        try:
            raw = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            raise CompatibilityError(f"Corrupt gaming layers manifest: {exc}") from exc
        if isinstance(raw, dict):
            entries = list(raw.get("dxvk_files", [])) + list(raw.get("vkd3d_files", []))
            for item in entries:
                if not isinstance(item, dict):
                    continue
                rel = str(item.get("relative_path", "")).strip()
                if not rel or rel.startswith("/") or ".." in Path(rel).parts:
                    continue
                target = (pfx / rel).resolve()
                if _is_within(target, pfx) and target.is_file():
                    target.unlink()
                    removed.append(rel)
        manifest_path.unlink()

    enforce_prefix_isolation(pfx)
    return {
        "profile": clean,
        "prefix_path": str(pfx),
        "removed_files": removed,
    }
