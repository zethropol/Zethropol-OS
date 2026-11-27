"""Unit tests for Vulkan/DXVK probing, renderer decisions, and isolated launch planning."""

from __future__ import annotations

import os
from pathlib import Path
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
from zethropol_compat.probe import (
    TranslationLayerProbe,
    VulkanProbeResult,
    WineRuntimeInfo,
    parse_vulkaninfo_summary,
    select_renderer,
)
from zethropol_compat.runner import SANDBOX_TARGET_MOUNT, build_launch_plan

SAMPLE_RX560_VULKAN = """
Devices:
========
GPU0:
\tapiVersion         = 1.4.354
\tdriverVersion      = 26.2.6
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = AMD Radeon RX 560 Series (RADV POLARIS11)
\tdriverName         = radv
"""

SAMPLE_LLVMPIPE_VULKAN = """
Devices:
========
GPU0:
\tapiVersion         = 1.4.354
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 21.1.0, 256 bits)
\tdriverName         = llvmpipe
"""

SAMPLE_LEGACY_VK12 = """
Devices:
========
GPU0:
\tapiVersion         = 1.2.199
\tdeviceType         = PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
\tdeviceName         = Intel HD Graphics 520
\tdriverName         = intel
"""


class ProbeAndRunnerTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="zethropol-compat-t2.")
        self._orig_xdg = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = self._tmp.name

    def tearDown(self) -> None:
        if self._orig_xdg is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._orig_xdg
        self._tmp.cleanup()

    def _seed_profile(self, name: str = "default-win64", renderer: str = "auto", sandbox: bool = True) -> Path:
        pfx = prefix_dir(name)
        (pfx / "drive_c" / "Program Files" / "Demo").mkdir(parents=True)
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
                renderer=renderer,
                locale="tr_TR.UTF-8",
                sandbox=sandbox,
                wine_binary="/usr/bin/wine",
                wine_version="wine-11.22",
                created_at="2026-11-28T00:00:00Z",
            ),
        )
        return pfx

    def test_vulkaninfo_parsing_distinguishes_hw_legacy_and_llvmpipe(self) -> None:
        hw = parse_vulkaninfo_summary(SAMPLE_RX560_VULKAN)
        self.assertTrue(hw.available)
        self.assertTrue(hw.hardware_vulkan_13)
        self.assertEqual(hw.devices[0].driver_name, "radv")

        sw = parse_vulkaninfo_summary(SAMPLE_LLVMPIPE_VULKAN)
        self.assertTrue(sw.available)
        self.assertFalse(sw.hardware_vulkan_13)

        old = parse_vulkaninfo_summary(SAMPLE_LEGACY_VK12)
        self.assertTrue(old.available)
        self.assertFalse(old.hardware_vulkan_13)

    def test_renderer_selection_matrix(self) -> None:
        hw_vk = parse_vulkaninfo_summary(SAMPLE_RX560_VULKAN)
        sw_vk = parse_vulkaninfo_summary(SAMPLE_LLVMPIPE_VULKAN)
        with_dxvk = TranslationLayerProbe(True, "system:/usr/share/dxvk/x64", True, "system:/usr/share/vkd3d-proton/x64")
        no_dxvk = TranslationLayerProbe(False, "none", False, "none")

        d1 = select_renderer("auto", hw_vk, with_dxvk)
        self.assertEqual(d1.effective, "dxvk")
        self.assertFalse(d1.fallback_applied)
        self.assertTrue(d1.vkd3d_enabled)
        self.assertIn("d3d11,dxgi=n,b", d1.dll_overrides)
        self.assertIn("d3d12,d3d12core=n,b", d1.dll_overrides)

        d2 = select_renderer("auto", hw_vk, no_dxvk)
        self.assertEqual(d2.effective, "wined3d")
        self.assertTrue(d2.fallback_applied)

        d3 = select_renderer("auto", sw_vk, with_dxvk)
        self.assertEqual(d3.effective, "wined3d")
        self.assertTrue(d3.fallback_applied)

        with self.assertRaises(CompatibilityError):
            select_renderer("dxvk", hw_vk, no_dxvk)

        d4 = select_renderer("wined3d", hw_vk, with_dxvk)
        self.assertEqual(d4.effective, "wined3d")
        self.assertFalse(d4.fallback_applied)

    def test_launch_plan_for_external_exe_and_msi_with_bwrap(self) -> None:
        pfx = self._seed_profile()
        ext_dir = Path(self._tmp.name) / "downloads"
        ext_dir.mkdir()
        exe_file = ext_dir / "installer.exe"
        exe_file.write_bytes(b"MZ")
        msi_file = ext_dir / "setup.msi"
        msi_file.write_bytes(b"\xd0\xcf\x11\xe0")
        bad_file = ext_dir / "script.sh"
        bad_file.write_text("echo nope\n", encoding="utf-8")

        wine_info = WineRuntimeInfo("/usr/bin/wine", "wine-11.22", False, True, "wow64-only")
        vk = parse_vulkaninfo_summary(SAMPLE_RX560_VULKAN)
        no_dxvk = TranslationLayerProbe(False, "none", False, "none")

        plan_exe = build_launch_plan(
            exe_file,
            extra_args=["/S"],
            profile="default-win64",
            vulkan_probe=vk,
            layer_probe=no_dxvk,
            bwrap_bin="/usr/bin/bwrap",
            wine_info=wine_info,
        )
        self.assertTrue(plan_exe.sandbox_enabled)
        self.assertEqual(plan_exe.target_windows_path, "Y:\\installer.exe")
        self.assertEqual(plan_exe.transient_drive_target, SANDBOX_TARGET_MOUNT)
        self.assertEqual(plan_exe.command[0], "/usr/bin/bwrap")
        self.assertIn("--ro-bind", plan_exe.command)
        self.assertIn(SANDBOX_TARGET_MOUNT, plan_exe.command)
        self.assertEqual(plan_exe.command[-3:], ["/usr/bin/wine", "Y:\\installer.exe", "/S"])
        self.assertEqual(plan_exe.env_overrides["LANG"], "tr_TR.UTF-8")

        plan_msi = build_launch_plan(
            msi_file,
            profile="default-win64",
            sandbox_override=False,
            vulkan_probe=vk,
            layer_probe=no_dxvk,
            bwrap_bin="/usr/bin/bwrap",
            wine_info=wine_info,
        )
        self.assertFalse(plan_msi.sandbox_enabled)
        self.assertEqual(plan_msi.target_kind, "msi")
        self.assertEqual(plan_msi.command, ["/usr/bin/wine", "msiexec", "/i", "Y:\\setup.msi"])

        # Executable inside drive_c uses native C:\ path with no transient drive
        in_c = pfx / "drive_c" / "Program Files" / "Demo" / "demo.exe"
        in_c.write_bytes(b"MZ")
        plan_c = build_launch_plan(
            in_c,
            profile="default-win64",
            vulkan_probe=vk,
            layer_probe=no_dxvk,
            bwrap_bin="/usr/bin/bwrap",
            wine_info=wine_info,
        )
        self.assertEqual(plan_c.target_windows_path, "C:\\Program Files\\Demo\\demo.exe")
        self.assertIsNone(plan_c.transient_drive_target)

        with self.assertRaises(CompatibilityError):
            build_launch_plan(bad_file, profile="default-win64", wine_info=wine_info)


if __name__ == "__main__":
    unittest.main()
