"""Stage 07 unit tests for GPU capability matrix, gaming readiness, and isolated prefix DXVK/VKD3D management."""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zethropol_compat.config import (
    CompatibilityError,
    ProfileMetadata,
    SCHEMA_VERSION,
    prefix_dir,
    write_profile_metadata,
)
from zethropol_compat.gaming import (
    ANTICHEAT_EULA_NOTICE,
    GAMING_LAYERS_MANIFEST,
    classify_gpu_capabilities,
    get_prefix_gaming_status,
    install_prefix_gaming_layers,
    probe_gaming_stack,
    remove_prefix_gaming_layers,
)
from zethropol_compat.probe import parse_vulkaninfo_summary

SERVICE_ROOT = Path(__file__).resolve().parent.parent
PREFIX_CTL = SERVICE_ROOT / "bin" / "zethropol-prefix-ctl"

SAMPLE_RX560 = """
Devices:
========
GPU0:
\tapiVersion         = 1.4.363
\tvendorID           = 0x1002
\tdeviceID           = 0x67ef
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = AMD Radeon RX 560 Series (RADV POLARIS11)
\tdriverName         = radv
\tdriverInfo         = Mesa 26.3.1
"""

SAMPLE_BROADWELL = """
Devices:
========
GPU0:
\tapiVersion         = 1.2.295
\tvendorID           = 0x8086
\tdeviceID           = 0x1616
\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
\tdeviceName         = Intel(R) HD Graphics 5500 (BDW GT2)
\tdriverName         = Intel open-source Mesa driver
\tdriverInfo         = Mesa 26.3.1 (hasvk)
"""

SAMPLE_VEGA3 = """
Devices:
========
GPU0:
\tapiVersion         = 1.3.296
\tvendorID           = 0x1002
\tdeviceID           = 0x15d8
\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
\tdeviceName         = AMD Radeon Vega 3 Graphics (RADV RAVEN2)
\tdriverName         = radv
\tdriverInfo         = Mesa 26.3.1
"""

SAMPLE_LLVMPIPE = """
Devices:
========
GPU0:
\tapiVersion         = 1.4.363
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 21.1.0, 256 bits)
\tdriverName         = llvmpipe
"""


class GamingLayerTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="zethropol-gaming.")
        self._root = Path(self._tmp.name)
        self._orig_xdg = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = str(self._root / "xdg-data")

    def tearDown(self) -> None:
        if self._orig_xdg is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._orig_xdg
        self._tmp.cleanup()

    def _seed_prefix(self, name: str = "gaming-win64") -> Path:
        pfx = prefix_dir(name)
        (pfx / "drive_c" / "windows" / "system32").mkdir(parents=True)
        (pfx / "drive_c" / "windows" / "syswow64").mkdir(parents=True)
        (pfx / "dosdevices").mkdir(parents=True)
        (pfx / "dosdevices" / "c:").symlink_to("../drive_c")
        write_profile_metadata(
            pfx,
            ProfileMetadata(
                schema_version=SCHEMA_VERSION,
                profile=name,
                template="default-win64",
                requested_arch="win64",
                effective_arch="win64",
                windows_version="win10",
                renderer="auto",
                locale="tr_TR.UTF-8",
                sandbox=True,
                wine_binary="/usr/bin/wine",
                wine_version="wine-11.22",
                created_at="2026-11-28T00:00:00Z",
            ),
        )
        return pfx

    def _seed_dll_sources(self) -> tuple[Path, Path, Path]:
        dxvk_x64 = self._root / "sys-dxvk" / "x64"
        dxvk_x32 = self._root / "sys-dxvk" / "x32"
        vkd3d_x64 = self._root / "sys-vkd3d" / "x64"
        dxvk_x64.mkdir(parents=True)
        dxvk_x32.mkdir(parents=True)
        vkd3d_x64.mkdir(parents=True)
        payload = b"MZ" + b"DXVK" * 5000
        for dll in ("d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"):
            (dxvk_x64 / dll).write_bytes(payload)
            (dxvk_x32 / dll).write_bytes(payload)
        for dll in ("d3d12.dll", "d3d12core.dll"):
            (vkd3d_x64 / dll).write_bytes(b"MZ" + b"VKD3D" * 4000)
        return (dxvk_x64, dxvk_x32, vkd3d_x64)

    def test_gpu_capability_matrix_covers_polaris11_broadwell_vega3_and_llvmpipe(self) -> None:
        rx560 = classify_gpu_capabilities(parse_vulkaninfo_summary(SAMPLE_RX560), vram_total_gb=4.0)
        self.assertEqual(rx560["matched_profile"], "amd-rx560-polaris11")
        self.assertTrue(rx560["dxvk_supported"])
        self.assertFalse(rx560["vkd3d_supported"])
        self.assertTrue(any("4.00 GB" in item for item in rx560["limitations"]))
        self.assertTrue(any("82 °C" in item for item in rx560["limitations"]))

        bdw = classify_gpu_capabilities(parse_vulkaninfo_summary(SAMPLE_BROADWELL))
        self.assertEqual(bdw["matched_profile"], "intel-broadwell-hd5500")
        self.assertFalse(bdw["hardware_vulkan_13"])
        self.assertFalse(bdw["dxvk_supported"])
        self.assertTrue(any("Broadwell" in item for item in bdw["limitations"]))

        vega3 = classify_gpu_capabilities(parse_vulkaninfo_summary(SAMPLE_VEGA3), vram_total_gb=1.0)
        self.assertEqual(vega3["matched_profile"], "amd-vega3-apu")
        self.assertTrue(vega3["dxvk_supported"])
        self.assertFalse(vega3["vkd3d_supported"])
        self.assertTrue(any("Vega 3" in item for item in vega3["limitations"]))

        sw = classify_gpu_capabilities(parse_vulkaninfo_summary(SAMPLE_LLVMPIPE))
        self.assertEqual(sw["matched_profile"], "software-vulkan")
        self.assertFalse(sw["dxvk_supported"])

    def test_gaming_stack_probe_reports_missing_components_and_anticheat_policy(self) -> None:
        vk = parse_vulkaninfo_summary(SAMPLE_RX560)
        report = probe_gaming_stack(
            vulkan_probe=vk,
            dxvk_dirs=(self._root / "missing-dxvk",),
            vkd3d_dirs=(self._root / "missing-vkd3d",),
            vram_total_gb=4.0,
        )
        self.assertEqual(report["gpu_capabilities"]["matched_profile"], "amd-rx560-polaris11")
        self.assertTrue(any("dxvk-mingw-git" in item for item in report["missing_components"]))
        self.assertTrue(report["anticheat_eula"]["kernel_anticheat_bypass_forbidden"])
        self.assertTrue(report["anticheat_eula"]["license_eula_compliance_required"])
        self.assertEqual(report["anticheat_eula"]["notice"], ANTICHEAT_EULA_NOTICE)

    def test_prefix_dxvk_install_enforces_notice_vulkan13_symlink_safety_and_clean_remove(self) -> None:
        pfx = self._seed_prefix("gaming-win64")
        dxvk_x64, dxvk_x32, vkd3d_x64 = self._seed_dll_sources()
        vk_ok = parse_vulkaninfo_summary(SAMPLE_RX560)
        vk_bdw = parse_vulkaninfo_summary(SAMPLE_BROADWELL)

        # 1. Refuses without explicit anti-cheat/EULA notice acceptance
        with self.assertRaises(CompatibilityError):
            install_prefix_gaming_layers(
                "gaming-win64",
                include_dxvk=True,
                accept_notice=False,
                vulkan_probe=vk_ok,
                dxvk_x64_dirs=(dxvk_x64,),
            )

        # 2. Refuses on legacy Vulkan 1.2 Broadwell hardware
        with self.assertRaises(CompatibilityError):
            install_prefix_gaming_layers(
                "gaming-win64",
                include_dxvk=True,
                accept_notice=True,
                vulkan_probe=vk_bdw,
                dxvk_x64_dirs=(dxvk_x64,),
            )

        # 3. Refuses symlink DLL injection attempt
        evil_dir = self._root / "evil-dxvk"
        evil_dir.mkdir()
        outside = self._root / "outside.bin"
        outside.write_bytes(b"SECRET")
        (evil_dir / "d3d11.dll").symlink_to(outside)
        with self.assertRaises(CompatibilityError):
            install_prefix_gaming_layers(
                "gaming-win64",
                include_dxvk=True,
                accept_notice=True,
                vulkan_probe=vk_ok,
                dxvk_x64_dirs=(evil_dir,),
            )

        # 4. Valid isolated installation (DXVK + VKD3D) preserves Z:\ isolation & records manifest
        res = install_prefix_gaming_layers(
            "gaming-win64",
            include_dxvk=True,
            include_vkd3d=True,
            accept_notice=True,
            vulkan_probe=vk_ok,
            dxvk_x64_dirs=(dxvk_x64,),
            dxvk_x32_dirs=(dxvk_x32,),
            vkd3d_x64_dirs=(vkd3d_x64,),
            vkd3d_x86_dirs=(self._root / "missing-v86",),
        )
        self.assertTrue(res["dxvk_installed"])
        self.assertTrue(res["vkd3d_installed"])
        self.assertEqual(len(res["dxvk_files"]), 8)  # 4 x64 + 4 x32
        self.assertEqual(len(res["vkd3d_files"]), 2)
        self.assertTrue((pfx / GAMING_LAYERS_MANIFEST).is_file())
        self.assertFalse((pfx / "dosdevices" / "z:").exists())

        st = get_prefix_gaming_status(
            "gaming-win64",
            vulkan_probe=vk_ok,
            dxvk_x64_dirs=(dxvk_x64,),
            vkd3d_x64_dirs=(vkd3d_x64,),
        )
        self.assertTrue(st["dxvk_installed"])
        self.assertTrue(st["vkd3d_installed"])
        self.assertTrue(st["managed_manifest_present"])

        # 5. Clean removal removes only tracked DLLs and manifest
        rm = remove_prefix_gaming_layers("gaming-win64")
        self.assertEqual(len(rm["removed_files"]), 10)
        self.assertFalse((pfx / GAMING_LAYERS_MANIFEST).exists())
        self.assertFalse((pfx / "drive_c" / "windows" / "system32" / "d3d11.dll").exists())

    def test_cli_gaming_probe_and_gaming_status_json(self) -> None:
        self._seed_prefix("gaming-win64")
        env = os.environ.copy()
        env["PYTHONDONTWRITEBYTECODE"] = "1"

        r_probe = subprocess.run(
            [sys.executable, "-B", str(PREFIX_CTL), "gaming-probe", "--json"],
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(r_probe.returncode, 0, msg=r_probe.stderr)
        probe_data = json.loads(r_probe.stdout)
        self.assertIn("gpu_capabilities", probe_data)
        self.assertIn("anticheat_eula", probe_data)
        self.assertTrue(probe_data["anticheat_eula"]["kernel_anticheat_bypass_forbidden"])

        r_status = subprocess.run(
            [sys.executable, "-B", str(PREFIX_CTL), "gaming-status", "gaming-win64", "--json"],
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(r_status.returncode, 0, msg=r_status.stderr)
        st_data = json.loads(r_status.stdout)
        self.assertEqual(st_data["profile"], "gaming-win64")
        self.assertFalse(st_data["dxvk_installed"])

        # install-dxvk without --accept-notice fails closed with exit 2
        r_fail = subprocess.run(
            [sys.executable, "-B", str(PREFIX_CTL), "install-dxvk", "gaming-win64", "--json"],
            env=env,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        self.assertEqual(r_fail.returncode, 2)


if __name__ == "__main__":
    unittest.main()
