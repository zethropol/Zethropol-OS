"""Unit tests for pure-Python MS-SHLLINK (.lnk) parsing and safe .desktop generation."""

from __future__ import annotations

import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from zethropol_compat.config import CompatibilityError, prefix_dir
from zethropol_compat.shortcut import (
    FLAG_HAS_ARGUMENTS,
    FLAG_HAS_LINK_INFO,
    FLAG_HAS_NAME,
    FLAG_IS_UNICODE,
    LNK_CLSID,
    LNK_HEADER_SIZE,
    convert_lnk_to_desktop,
    escape_desktop_exec_arg,
    parse_lnk,
    sync_prefix_shortcuts,
)


def build_synthetic_lnk(
    local_base_path: str,
    name: str = "",
    arguments: str = "",
    unicode_strings: bool = True,
) -> bytes:
    flags = FLAG_HAS_LINK_INFO
    if unicode_strings:
        flags |= FLAG_IS_UNICODE
    if name:
        flags |= FLAG_HAS_NAME
    if arguments:
        flags |= FLAG_HAS_ARGUMENTS

    header = bytearray(LNK_HEADER_SIZE)
    struct.pack_into("<I", header, 0, LNK_HEADER_SIZE)
    header[4:20] = LNK_CLSID
    struct.pack_into("<I", header, 20, flags)

    # LinkInfo (header size 0x24 with Unicode local base path)
    li_header_size = 0x24
    vol_id_offset = li_header_size
    vol_id = struct.pack("<IIII", 16, 3, 0, 0)  # 16-byte minimal VolumeID
    ansi_base_off = vol_id_offset + len(vol_id)
    ansi_base = local_base_path.encode("cp1254", errors="replace") + b"\x00"
    common_suffix_off = ansi_base_off + len(ansi_base)
    common_suffix = b"\x00"
    uni_base_off = common_suffix_off + len(common_suffix)
    uni_base = local_base_path.encode("utf-16le") + b"\x00\x00"
    uni_suffix_off = uni_base_off + len(uni_base)
    uni_suffix = b"\x00\x00"

    li_body = vol_id + ansi_base + common_suffix + uni_base + uni_suffix
    li_size = li_header_size + len(li_body)
    li_header = struct.pack(
        "<IIIIIIIII",
        li_size,
        li_header_size,
        0x01,  # VolumeIDAndLocalBasePath
        vol_id_offset,
        ansi_base_off,
        0,
        common_suffix_off,
        uni_base_off,
        uni_suffix_off,
    )

    out = bytearray(header)
    out.extend(li_header)
    out.extend(li_body)

    def append_str(s: str) -> None:
        if unicode_strings:
            out.extend(struct.pack("<H", len(s)))
            out.extend(s.encode("utf-16le"))
        else:
            raw = s.encode("cp1254")
            out.extend(struct.pack("<H", len(raw)))
            out.extend(raw)

    if name:
        append_str(name)
    if arguments:
        append_str(arguments)

    return bytes(out)


class LnkConverterTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="zethropol-compat-t3.")
        self._orig_xdg = os.environ.get("XDG_DATA_HOME")
        os.environ["XDG_DATA_HOME"] = self._tmp.name

    def tearDown(self) -> None:
        if self._orig_xdg is None:
            os.environ.pop("XDG_DATA_HOME", None)
        else:
            os.environ["XDG_DATA_HOME"] = self._orig_xdg
        self._tmp.cleanup()

    def test_parses_unicode_turkish_lnk_and_emits_escaped_desktop(self) -> None:
        pfx = prefix_dir("default-win64")
        app_dir = pfx / "drive_c" / "Program Files" / "Türkçe Uygulama"
        app_dir.mkdir(parents=True)
        exe_path = app_dir / "Örnek $App%.exe"
        exe_path.write_bytes(b"MZ")

        desktop_dir = pfx / "drive_c" / "users" / "tester" / "Desktop"
        desktop_dir.mkdir(parents=True)
        lnk_path = desktop_dir / "Örnek Uygulama.lnk"
        lnk_path.write_bytes(
            build_synthetic_lnk(
                local_base_path="C:\\Program Files\\Türkçe Uygulama\\Örnek $App%.exe",
                name="Türkçe Çalıştırıcı",
                arguments='--mode="fast"',
                unicode_strings=True,
            )
        )

        parsed = parse_lnk(lnk_path)
        self.assertEqual(parsed.name, "Türkçe Çalıştırıcı")
        self.assertEqual(parsed.target_windows_path, "C:\\Program Files\\Türkçe Uygulama\\Örnek $App%.exe")

        out_desktop = convert_lnk_to_desktop(lnk_path, profile="default-win64")
        self.assertTrue(out_desktop.is_file())
        content = out_desktop.read_text(encoding="utf-8")
        self.assertIn("Name=Türkçe Çalıştırıcı", content)
        self.assertIn("X-Zethropol-Managed=true", content)
        self.assertIn("X-Zethropol-Profile=default-win64", content)
        # Verify FreeDesktop Exec escaping of $, %, and quotes
        self.assertIn("\\$App%%.exe", content)
        self.assertIn('\\"--mode=\\"fast\\"\\"' if False else '--mode=\\"fast\\"', content)

    def test_rejects_z_drive_and_traversal_targets(self) -> None:
        pfx = prefix_dir("default-win64")
        desktop_dir = pfx / "drive_c" / "users" / "tester" / "Desktop"
        desktop_dir.mkdir(parents=True)

        z_lnk = desktop_dir / "escape_z.lnk"
        z_lnk.write_bytes(build_synthetic_lnk("Z:\\usr\\bin\\sh.exe"))
        with self.assertRaises(CompatibilityError):
            convert_lnk_to_desktop(z_lnk, profile="default-win64")

        trav_lnk = desktop_dir / "escape_trav.lnk"
        trav_lnk.write_bytes(build_synthetic_lnk("C:\\..\\..\\etc\\passwd.exe"))
        with self.assertRaises(CompatibilityError):
            convert_lnk_to_desktop(trav_lnk, profile="default-win64")

        # sync_prefix_shortcuts skips invalid shortcuts without failing valid ones
        good_exe = pfx / "drive_c" / "app.exe"
        good_exe.write_bytes(b"MZ")
        good_lnk = desktop_dir / "good.lnk"
        good_lnk.write_bytes(build_synthetic_lnk("C:\\app.exe", name="Good App", unicode_strings=False))

        report = sync_prefix_shortcuts("default-win64")
        self.assertEqual(len(report["converted"]), 1)
        self.assertEqual(len(report["skipped"]), 2)

    def test_exec_arg_escaping_rejects_control_chars(self) -> None:
        self.assertEqual(escape_desktop_exec_arg("a$b%c`d"), '"a\\$b%%c\\`d"')
        with self.assertRaises(CompatibilityError):
            escape_desktop_exec_arg("bad\ninjection")


if __name__ == "__main__":
    unittest.main()
