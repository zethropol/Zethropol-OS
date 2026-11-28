"""Unit and real cross-service private D-Bus tests for ZciDaemonClient (Stage 10).

Spawns BOTH the real `services/security` (`org.zethropol.Security.Policy.v1`) and
real `services/zci-daemon` (`org.zethropol.ZciDaemon.v1`) binaries on an isolated
private `dbus-daemon` to prove end-to-end policy gating, untrusted sealing,
passive standby lifecycle, lockout sync, and Polkit admin boundaries.
"""
from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

from zci_daemon_client import ZciDaemonClient


def _find_binary(rel_candidates: tuple[str, ...]) -> Path | None:
    for rel in rel_candidates:
        p = REPO_ROOT / rel
        if p.is_file() and os.access(p, os.X_OK):
            return p
    return None


class ZciDaemonClientFailClosedTests(unittest.TestCase):
    def test_fail_closed_when_daemon_unreachable(self) -> None:
        client = ZciDaemonClient(
            bus_address="unix:path=/tmp/nonexistent-zethropol-zci10.sock",
            timeout_secs=2.0,
        )
        res = client.execute_action("sid", "zci-chat", "model_query", "inspect_untrusted")
        self.assertFalse(res.success)
        self.assertEqual(res.code, "zci-daemon-unavailable")

        wrap = client.wrap_untrusted_content("zci-chat", "user_text", "hello")
        self.assertFalse(wrap.success)
        self.assertEqual(wrap.code, "zci-daemon-unavailable")

    def test_rejects_relative_busctl_path(self) -> None:
        with self.assertRaises(ValueError):
            ZciDaemonClient(busctl_bin="busctl")


class ZciDaemonCrossServiceDbusTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sec_bin = _find_binary((
            "services/security/target/release/security",
            "services/security/target/debug/security",
        ))
        self.daemon_bin = _find_binary((
            "services/zci-daemon/target/release/zci-daemon",
            "services/zci-daemon/target/debug/zci-daemon",
        ))
        if self.sec_bin is None or self.daemon_bin is None:
            self.skipTest("Compiled security and zci-daemon binaries required for cross-service test")

        self.tmp = tempfile.TemporaryDirectory(prefix="zci-daemon-e2e-")
        self.root = Path(self.tmp.name)
        self.fake_home = self.root / "home"
        self.ws = self.fake_home / "Zethropol-OS"
        (self.ws / "src").mkdir(parents=True)
        (self.ws / "src" / "hello.rs").write_text("fn main() {}\n", encoding="utf-8")
        (self.ws / "Cargo.toml").write_text('[package]\nname = "demo"\n', encoding="utf-8")
        (self.ws / "AGENTS.md").write_text("# lock\n", encoding="utf-8")

        self.pk_allow_flag = self.root / "pk-allow"
        self.mock_pkcheck = self.root / "mock-pkcheck.sh"
        self.mock_pkcheck.write_text(
            f'#!/usr/bin/env bash\nif [ -f "{self.pk_allow_flag}" ]; then exit 0; else exit 1; fi\n',
            encoding="utf-8",
        )
        self.mock_pkcheck.chmod(0o755)

        self.dbus_proc = subprocess.Popen(
            ["/usr/bin/dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert self.dbus_proc.stdout is not None
        self.bus_addr = self.dbus_proc.stdout.readline().strip()
        self.assertTrue(self.bus_addr)

        self.sec_proc = subprocess.Popen(
            [
                str(self.sec_bin),
                "--private-bus",
                self.bus_addr,
                "--state-file",
                str(self.root / "state" / "zci-policy-state.json"),
                "--audit-log",
                str(self.root / "log" / "zci-audit.jsonl"),
                "--home-override",
                str(self.fake_home),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.daemon_proc = subprocess.Popen(
            [
                str(self.daemon_bin),
                "--private-bus",
                self.bus_addr,
                "--pkcheck-bin",
                str(self.mock_pkcheck),
                "--config-file",
                str(self.root / "zci-daemon.json"),
                "--model-gateway",
                "127.0.0.1:29999",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        self.client = ZciDaemonClient(bus_address=self.bus_addr, timeout_secs=5.0)
        deadline = time.monotonic() + 6.0
        while time.monotonic() < deadline:
            st = self.client.get_daemon_status()
            if st.success and st.data.get("policy_engine_reachable") is True:
                return
            time.sleep(0.05)
        self.fail("Real security + zci-daemon did not become ready on private D-Bus")

    def tearDown(self) -> None:
        if getattr(self, "dbus_proc", None) is not None and self.dbus_proc.stdout is not None:
            self.dbus_proc.stdout.close()
        for proc in (
            getattr(self, "daemon_proc", None),
            getattr(self, "sec_proc", None),
            getattr(self, "dbus_proc", None),
        ):
            if proc is not None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=3)
        if hasattr(self, "tmp"):
            self.tmp.cleanup()

    def test_real_security_and_zci_daemon_end_to_end(self) -> None:
        # 1. Health & passive standby status
        st = self.client.get_daemon_status()
        self.assertTrue(st.success)
        self.assertEqual(st.code, "ok")
        self.assertTrue(st.data["policy_engine_reachable"])
        self.assertTrue(st.data["policy_default_deny"])
        self.assertTrue(st.data["policy_self_mod_lock"])
        self.assertEqual(st.data["passive_model_state"], "Standby")
        self.assertEqual(st.data["passive_model_estimated_vram_mb"], 0)

        # 2. Open zekans-code session, read file & analyze project (sealed by real WrapUntrustedData!)
        opened = self.client.open_session("zekans-code")
        self.assertTrue(opened.success)
        sid = opened.data["session_id"]

        rd = self.client.execute_action(
            sid, "zekans-code", "read_file", "read_file", str(self.ws / "src" / "hello.rs")
        )
        self.assertTrue(rd.success)
        self.assertFalse(rd.data["executable"])
        self.assertEqual(rd.data["policy_authority"], "none")
        self.assertIn("fn main()", rd.data["wrapped_payload"])

        proj = self.client.execute_action(
            sid, "zekans-code", "project_analysis", "list_dir", str(self.ws)
        )
        self.assertTrue(proj.success)
        self.assertIn("Cargo.toml", proj.data["wrapped_payload"])
        self.assertNotIn("AGENTS.md", proj.data["wrapped_payload"])

        # 3. Read-only system diagnostic against real org.zethropol.Security.Policy.v1.GetPolicySummary
        diag = self.client.execute_action(
            sid,
            "zekans-code",
            "system_diagnostics",
            "dbus_call",
            "GetPolicySummary",
            dbus_interface="org.zethropol.Security.Policy.v1",
        )
        self.assertTrue(diag.success)
        self.assertIn("0.1.0-stage08", diag.data["wrapped_payload"])

        # 4. Self-modification attack on AGENTS.md -> blocked by real Security Engine & session LockedOut!
        sm = self.client.execute_action(
            sid, "zekans-code", "userspace_preflight", "write_file", str(self.ws / "AGENTS.md")
        )
        self.assertFalse(sm.success)
        self.assertEqual(sm.code, "self-modification-denied")
        self.assertTrue(sm.data["lockout_active"])

        # 5. Polkit admin boundary on TerminateSession
        deny_term = self.client.terminate_session(sid, "no-polkit")
        self.assertFalse(deny_term.success)
        self.assertEqual(deny_term.code, "permission-denied")

        self.pk_allow_flag.write_text("allow", encoding="utf-8")
        ok_term = self.client.terminate_session(sid, "admin-ok")
        self.assertTrue(ok_term.success)
        self.assertEqual(ok_term.data["terminated_count"], 1)


if __name__ == "__main__":
    unittest.main()
