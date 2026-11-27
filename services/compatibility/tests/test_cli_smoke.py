"""CLI and desktop-file smoke tests using isolated temp XDG_DATA_HOME and mock Wine."""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

SERVICE_ROOT = Path(__file__).resolve().parent.parent
PREFIX_CTL = SERVICE_ROOT / "bin" / "zethropol-prefix-ctl"
WINE_RUN = SERVICE_ROOT / "bin" / "zethropol-wine-run"
DESKTOP_FILE = SERVICE_ROOT / "desktop" / "org.zethropol.wine-run.desktop"


class CliSmokeTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="zethropol-compat-cli.")
        self._root = Path(self._tmp.name)
        self._env = os.environ.copy()
        self._env["XDG_DATA_HOME"] = str(self._root / "xdg-data")
        self._env["PYTHONDONTWRITEBYTECODE"] = "1"

        # Build a lightweight mock Wine runtime to test full CLI lifecycle offline & headlessly
        mock_bin_dir = self._root / "mock-wine" / "bin"
        (self._root / "mock-wine" / "lib" / "wine" / "x86_64-unix").mkdir(parents=True)
        (self._root / "mock-wine" / "lib" / "wine" / "x86_64-windows").mkdir(parents=True)
        (self._root / "mock-wine" / "lib" / "wine" / "i386-windows").mkdir(parents=True)
        mock_bin_dir.mkdir(parents=True)

        self._mock_wine = mock_bin_dir / "wine"
        self._mock_wine.write_text(
            "#!/usr/bin/env bash\n"
            "if [ \"${1:-}\" = \"--version\" ]; then echo 'wine-11.22'; exit 0; fi\n"
            "if [ \"${1:-}\" = \"regedit\" ] && [ \"${2:-}\" = \"/S\" ]; then\n"
            "  cat \"$3\" >> \"$WINEPREFIX/user.reg\"\n"
            "  cat \"$3\" >> \"$WINEPREFIX/system.reg\"\n"
            "  exit 0\n"
            "fi\n"
            "exit 0\n",
            encoding="utf-8",
        )
        self._mock_wine.chmod(0o755)

        mock_wineboot = mock_bin_dir / "wineboot"
        mock_wineboot.write_text(
            "#!/usr/bin/env bash\n"
            "mkdir -p \"$WINEPREFIX/dosdevices\" \"$WINEPREFIX/drive_c/users/tester\"\n"
            "ln -sfn ../drive_c \"$WINEPREFIX/dosdevices/c:\"\n"
            "ln -sfn / \"$WINEPREFIX/dosdevices/z:\"\n"
            "ln -sfn \"$HOME\" \"$WINEPREFIX/drive_c/users/tester/Documents\"\n"
            "touch \"$WINEPREFIX/user.reg\" \"$WINEPREFIX/system.reg\"\n"
            "exit 0\n",
            encoding="utf-8",
        )
        mock_wineboot.chmod(0o755)

        mock_wineserver = mock_bin_dir / "wineserver"
        mock_wineserver.write_text("#!/usr/bin/env bash\nexit 0\n", encoding="utf-8")
        mock_wineserver.chmod(0o755)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, script: Path, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, "-B", str(script), *args],
            env=self._env,
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def test_full_prefix_lifecycle_and_dry_run_runner(self) -> None:
        # 1. Init classic-win32 on mock WoW64-only Wine -> must adapt cleanly to win64-wow64 & pass audit!
        r_init = self._run(
            PREFIX_CTL,
            "init",
            "classic-win32",
            "--template",
            "classic-win32",
            "--wine-bin",
            str(self._mock_wine),
            "--json",
        )
        self.assertEqual(r_init.returncode, 0, msg=r_init.stderr + r_init.stdout)
        init_data = json.loads(r_init.stdout)
        self.assertEqual(init_data["metadata"]["effective_arch"], "win64-wow64")
        self.assertEqual(init_data["metadata"]["windows_version"], "win7")
        self.assertTrue(init_data["audit"]["passed"])

        # 2. Audit & list & info
        r_list = self._run(PREFIX_CTL, "list", "--json")
        self.assertEqual(r_list.returncode, 0)
        self.assertEqual(len(json.loads(r_list.stdout)["profiles"]), 1)

        r_audit = self._run(PREFIX_CTL, "audit", "classic-win32", "--json")
        self.assertEqual(r_audit.returncode, 0)
        self.assertTrue(json.loads(r_audit.stdout)["passed"])

        # 3. Runner dry-run on .exe and .msi
        sample_exe = self._root / "app.exe"
        sample_exe.write_bytes(b"MZ")
        r_dry = self._run(
            WINE_RUN,
            "--profile",
            "classic-win32",
            "--wine-bin",
            str(self._mock_wine),
            "--dry-run",
            "--json",
            "--",
            str(sample_exe),
        )
        self.assertEqual(r_dry.returncode, 0, msg=r_dry.stderr)
        plan = json.loads(r_dry.stdout)
        self.assertEqual(plan["effective_arch"], "win64-wow64")
        self.assertEqual(plan["renderer"]["effective"], "wined3d")
        self.assertEqual(plan["target_windows_path"], "Y:\\app.exe")

        # 4. Remove requires --yes
        r_rm_no = self._run(PREFIX_CTL, "remove", "classic-win32")
        self.assertNotEqual(r_rm_no.returncode, 0)
        r_rm_yes = self._run(PREFIX_CTL, "remove", "classic-win32", "--yes", "--json")
        self.assertEqual(r_rm_yes.returncode, 0, msg=r_rm_yes.stderr)

    def test_desktop_entry_passes_desktop_file_validate(self) -> None:
        self.assertTrue(DESKTOP_FILE.is_file())
        validator = shutil.which("desktop-file-validate")
        if validator:
            res = subprocess.run([validator, str(DESKTOP_FILE)], capture_output=True, text=True, check=False)
            self.assertEqual(res.returncode, 0, msg=res.stdout + res.stderr)


if __name__ == "__main__":
    unittest.main()
