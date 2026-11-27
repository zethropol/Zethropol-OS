"""Unit tests for profile validation, Wine 11 WoW64 adaptation, registry generation, and isolation."""

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
    applications_dir,
    prefix_dir,
    validate_profile_name,
    write_profile_metadata,
)
from zethropol_compat.prefix import (
    audit_prefix,
    build_zethropol_registry,
    enforce_prefix_isolation,
    remove_profile,
    resolve_effective_arch,
)
from zethropol_compat.probe import WineRuntimeInfo


class PrefixControlTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="zethropol-compat-t1.")
        self._orig_xdg = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = self._tmp.name

    def tearDown(self) -> None:
        if self._orig_xdg is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._orig_xdg
        self._tmp.cleanup()

    def test_profile_name_validation_rejects_traversal_and_bad_chars(self) -> None:
        for good in ("default-win64", "classic-win32", "office_2026", "a"):
            self.assertEqual(validate_profile_name(good), good)
        for bad in ("", "../escape", "a/b", "Upper", "-dash", ".hidden", "has space", "a" * 49):
            with self.assertRaises(CompatibilityError):
                validate_profile_name(bad)

    def test_classic_win32_adapts_safely_on_wine11_wow64_only(self) -> None:
        wow64_wine = WineRuntimeInfo(
            binary="/usr/bin/wine",
            version="wine-11.22",
            supports_native_win32=False,
            supports_wow64=True,
            mode="wow64-only",
        )
        multiarch_wine = WineRuntimeInfo(
            binary="/usr/bin/wine",
            version="wine-10.0",
            supports_native_win32=True,
            supports_wow64=True,
            mode="multiarch",
        )
        self.assertEqual(resolve_effective_arch("win32", wow64_wine), ("win64", "win64-wow64"))
        self.assertEqual(resolve_effective_arch("win64", wow64_wine), ("win64", "win64"))
        self.assertEqual(resolve_effective_arch("win32", multiarch_wine), ("win32", "win32"))

    def test_registry_contains_turkish_locale_fonts_and_disabled_menubuilder(self) -> None:
        reg64 = build_zethropol_registry("default-win64")
        self.assertIn("Windows Registry Editor Version 5.00", reg64)
        self.assertIn('"Locale"="0000041F"', reg64)
        self.assertIn('"LocaleName"="tr-TR"', reg64)
        self.assertIn('"Arial"="Liberation Sans"', reg64)
        self.assertIn('"Times New Roman"="Liberation Serif"', reg64)
        self.assertIn('"Courier New"="Liberation Mono"', reg64)
        self.assertIn('"MS Shell Dlg 2"="DejaVu Sans"', reg64)
        self.assertIn('"winemenubuilder.exe"=""', reg64)
        self.assertIn('"Version"="win10"', reg64)

        reg32 = build_zethropol_registry("classic-win32")
        self.assertIn('"Version"="win7"', reg32)
        self.assertIn('"ProductName"="Microsoft Windows 7"', reg32)

    def test_enforce_isolation_removes_z_drive_and_replaces_home_symlinks(self) -> None:
        pfx = prefix_dir("default-win64")
        (pfx / "dosdevices").mkdir(parents=True)
        (pfx / "drive_c" / "users" / "tester").mkdir(parents=True)
        (pfx / "dosdevices" / "c:").symlink_to("../drive_c")
        (pfx / "dosdevices" / "z:").symlink_to("/")
        (pfx / "dosdevices" / "h:").symlink_to(Path.home())
        (pfx / "drive_c" / "users" / "tester" / "Documents").symlink_to(Path.home())
        (pfx / "drive_c" / "users" / "tester" / "Desktop").symlink_to(Path.home())

        (pfx / "user.reg").write_text(
            '"Locale"="0000041F"\n"winemenubuilder.exe"=""\n"Arial"="Liberation Sans"\n',
            encoding="utf-8",
        )
        (pfx / "system.reg").write_text('"Arial"="Liberation Sans"\n', encoding="utf-8")
        write_profile_metadata(
            pfx,
            ProfileMetadata(
                schema_version=SCHEMA_VERSION,
                profile="default-win64",
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

        # Before isolation, audit must fail on Z: and user folder symlinks
        before = audit_prefix("default-win64")
        self.assertFalse(before.passed)

        changes = enforce_prefix_isolation(pfx)
        self.assertIn("z:", changes["removed_drives"])
        self.assertIn("h:", changes["removed_drives"])
        self.assertFalse((pfx / "dosdevices" / "z:").exists())
        self.assertFalse((pfx / "dosdevices" / "z:").is_symlink())
        self.assertTrue((pfx / "dosdevices" / "c:").is_symlink())

        docs = pfx / "drive_c" / "users" / "tester" / "Documents"
        self.assertTrue(docs.is_dir())
        self.assertFalse(docs.is_symlink())

        after = audit_prefix("default-win64")
        self.assertTrue(after.passed, msg=after.to_dict())

    def test_remove_profile_requires_confirm_and_only_deletes_managed_shortcuts(self) -> None:
        pfx = prefix_dir("office")
        pfx.mkdir(parents=True)
        apps = applications_dir()
        apps.mkdir(parents=True)

        managed = apps / "zethropol-wine-office-app-12345678.desktop"
        managed.write_text("X-Zethropol-Managed=true\nX-Zethropol-Profile=office\n", encoding="utf-8")
        unmanaged = apps / "zethropol-wine-office-user-custom.desktop"
        unmanaged.write_text("[Desktop Entry]\nName=Custom\n", encoding="utf-8")

        with self.assertRaises(CompatibilityError):
            remove_profile("office", confirm=False)
        self.assertTrue(pfx.exists())

        res = remove_profile("office", confirm=True)
        self.assertFalse(pfx.exists())
        self.assertFalse(managed.exists())
        self.assertTrue(unmanaged.exists())
        self.assertEqual(res["removed_desktop_files"], [str(managed)])


if __name__ == "__main__":
    unittest.main()
