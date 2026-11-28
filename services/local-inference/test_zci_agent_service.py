"""Real 4-service cross-process private D-Bus integration test for ZCI Command Agent (Stage 13).

Spawns ALL FOUR real Rust services on ONE isolated private `dbus-daemon`:
  1. `services/security` (`org.zethropol.Security.Policy.v1`)
  2. `services/zci-daemon` (`org.zethropol.ZciDaemon.v1`)
  3. `services/zci-runtime` (`org.zethropol.ZciRuntime.v1`)
  4. `services/zci-agent-service` (`org.zethropol.ZciAgent.v1`)
Plus a local `127.0.0.1` loopback mock structured-planner HTTP backend, AND invokes the REAL
compiled Qt6 `desktop/zci-agent/build/zci-agent` CLI (`--status`, `--plan`, `--approve`,
`--execute`, `--audit`) AND `zci-code-gemma4-shared agent --status` wrapper to verify end-to-end
Stage 13 guarantees (`13.01`–`13.07`).
"""
from __future__ import annotations

import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))

from zci_runtime_client import ZciRuntimeClient


def _find_binary(rel_candidates: tuple[str, ...]) -> Path | None:
    for rel in rel_candidates:
        p = REPO_ROOT / rel
        if p.is_file() and os.access(p, os.X_OK):
            return p
    return None


class _MockLoopbackPlannerHandler(BaseHTTPRequestHandler):
    workspace_dir: str = ""
    target_file: str = ""

    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        _body = self.rfile.read(length).decode("utf-8", errors="replace")
        plan_obj = {
            "summary": "Proje analizi ve onaylı rapor dosyası oluşturma planı",
            "steps": [
                {
                    "action_kind": "project_analysis",
                    "target": self.workspace_dir,
                    "description": "Çalışma alanını salt-okunur analiz et",
                },
                {
                    "action_kind": "recovery_readiness",
                    "description": "Snapper ve kurtarma kilidi hazırlığını doğrula",
                },
                {
                    "action_kind": "create_workspace_file",
                    "target": self.target_file,
                    "content": "Zethropol Stage 13 onaylı ajan çıktısı.\n",
                    "description": "Kullanıcı onaylı çalışma alanı dosyası oluştur",
                },
            ],
        }
        payload = json.dumps(
            {"choices": [{"message": {"content": json.dumps(plan_obj)}}]}
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: object) -> None:
        return


class ZciAgentFourServiceIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.sec_bin = _find_binary((
            "services/security/target/release/security",
            "services/security/target/debug/security",
        ))
        self.daemon_bin = _find_binary((
            "services/zci-daemon/target/release/zci-daemon",
            "services/zci-daemon/target/debug/zci-daemon",
        ))
        self.runtime_bin = _find_binary((
            "services/zci-runtime/target/release/zci-runtime",
            "services/zci-runtime/target/debug/zci-runtime",
        ))
        self.agentd_bin = _find_binary((
            "services/zci-agent-service/target/release/zci-agent-service",
            "services/zci-agent-service/target/debug/zci-agent-service",
        ))
        self.cli_bin = _find_binary(("desktop/zci-agent/build/zci-agent",))

        if not (
            self.sec_bin
            and self.daemon_bin
            and self.runtime_bin
            and self.agentd_bin
            and self.cli_bin
        ):
            self.skipTest(
                "Compiled security, zci-daemon, zci-runtime, zci-agent-service, and zci-agent binaries required"
            )

        self.addCleanup(self._cleanup_all)
        self.tmp = tempfile.TemporaryDirectory(prefix="zci-agent-4svc-")
        self.root = Path(self.tmp.name)
        self.fake_home = self.root / "home"
        self.workspace = self.fake_home / "Projects" / "stage13-demo"
        self.workspace.mkdir(parents=True)
        (self.workspace / "Cargo.toml").write_text('[package]\nname = "demo"\n', encoding="utf-8")
        self.target_file = self.workspace / "agent-approved-output.txt"
        self.agent_storage = self.root / "agent-store"

        gguf = self.root / "gemma-4-E4B-it-Q4_K_M.gguf"
        gguf_bytes = b"GGUF\x03\x00\x00\x00zethropol-stage13-agent-weights"
        gguf.write_bytes(gguf_bytes)
        manifest = self.root / "manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "models": [
                        {
                            "model_id": "gemma-4-E4B-it-Q4_K_M",
                            "display_name": "Gemma 4 E4B Instruct",
                            "format": "gguf",
                            "path": str(gguf),
                            "size_bytes": len(gguf_bytes),
                            "sha256": hashlib.sha256(gguf_bytes).hexdigest(),
                            "license": "Apache-2.0",
                            "min_profile": "dev-baseline",
                            "required_ram_mb": 4096,
                            "required_vram_mb": 0,
                            "hybrid_vram_estimate_mb": 1536,
                            "max_context_tokens": 8192,
                            "trusted": True,
                            "signature_status": "verified-manifest-pin",
                        }
                    ],
                }
            ),
            encoding="utf-8",
        )

        self.pk_flag = self.root / "pk-allow"
        self.pk_flag.write_text("allow", encoding="utf-8")
        self.mock_pkcheck = self.root / "mock-pkcheck.sh"
        self.mock_pkcheck.write_text(
            f'#!/usr/bin/env bash\nif [ -f "{self.pk_flag}" ]; then exit 0; else exit 1; fi\n',
            encoding="utf-8",
        )
        self.mock_pkcheck.chmod(0o755)

        handler_cls = type(
            "_BoundPlannerHandler",
            (_MockLoopbackPlannerHandler,),
            {
                "workspace_dir": str(self.workspace),
                "target_file": str(self.target_file),
            },
        )
        self.httpd = HTTPServer(("127.0.0.1", 0), handler_cls)
        self.engine_port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

        self.dbus_proc = subprocess.Popen(
            ["/usr/bin/dbus-daemon", "--session", "--nofork", "--nopidfile", "--print-address=1"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert self.dbus_proc.stdout is not None
        self.bus_addr = self.dbus_proc.stdout.readline().strip()

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
                f"127.0.0.1:{self.engine_port}",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.runtime_proc = subprocess.Popen(
            [
                str(self.runtime_bin),
                "--private-bus",
                self.bus_addr,
                "--pkcheck-bin",
                str(self.mock_pkcheck),
                "--manifest-file",
                str(manifest),
                "--engine-endpoint",
                f"127.0.0.1:{self.engine_port}",
                "--idle-sleep-secs",
                "30",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        self.agentd_proc = subprocess.Popen(
            [
                str(self.agentd_bin),
                "--private-bus",
                self.bus_addr,
                "--storage-dir",
                str(self.agent_storage),
                "--home-override",
                str(self.fake_home),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        self.rt_client = ZciRuntimeClient(bus_address=self.bus_addr, timeout_secs=5.0)
        ready = False
        for _ in range(50):
            rt_st = self.rt_client.get_runtime_status()
            if (
                rt_st.success
                and rt_st.data.get("policy_engine_reachable")
                and rt_st.data.get("daemon_bridge_reachable")
            ):
                loaded = self.rt_client.load_model("gemma-4-E4B-it-Q4_K_M")
                if loaded.success:
                    cli_st = self._run_cli("--status")
                    if cli_st.returncode == 0 and json.loads(cli_st.stdout).get("code") == "ok":
                        ready = True
                        break
            time.sleep(0.1)
        if not ready:
            self.fail("4-service private D-Bus stack did not become ready in time")

    def _cleanup_all(self) -> None:
        if getattr(self, "dbus_proc", None) is not None and self.dbus_proc.stdout is not None:
            try:
                self.dbus_proc.stdout.close()
            except OSError:
                pass
        for proc in (
            getattr(self, "agentd_proc", None),
            getattr(self, "runtime_proc", None),
            getattr(self, "daemon_proc", None),
            getattr(self, "sec_proc", None),
            getattr(self, "dbus_proc", None),
        ):
            if proc is not None and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
        if getattr(self, "httpd", None) is not None:
            self.httpd.shutdown()
            self.httpd.server_close()
            self.httpd = None
        if getattr(self, "tmp", None) is not None:
            self.tmp.cleanup()
            self.tmp = None

    def _run_cli(self, *args: str) -> subprocess.CompletedProcess[str]:
        assert self.cli_bin is not None
        return subprocess.run(
            [str(self.cli_bin), "--bus-address", self.bus_addr, *args],
            capture_output=True,
            text=True,
            timeout=25,
            check=False,
        )

    def test_four_service_real_dbus_agent_cli_approval_and_audit_chain(self) -> None:
        # 1. --status reports locked safety invariants & strict 0700/0600 audit store
        st_proc = self._run_cli("--status")
        self.assertEqual(st_proc.returncode, 0, st_proc.stderr)
        st = json.loads(st_proc.stdout)
        self.assertTrue(st["success"])
        st_d = st["data"]
        self.assertFalse(st_d["raw_shell_execution_enabled"])
        self.assertFalse(st_d["rollback_execution_enabled"])
        self.assertFalse(st_d["repair_execution_enabled"])
        self.assertTrue(st_d["loopback_only"])
        self.assertTrue(st_d["audit_chain_valid"])
        self.assertTrue(st_d["audit_permissions_strict"])
        self.assertTrue(st_d["zekans_code_aligned"])

        # 2. Raw shell injection & protected system paths rejected fail-closed
        sh_proc = self._run_cli(
            "--plan", "sh -c 'cat /etc/shadow'", "--profile", "zekans-code", "--workspace", str(self.workspace)
        )
        self.assertNotEqual(sh_proc.returncode, 0)
        sh_res = json.loads(sh_proc.stdout)
        self.assertFalse(sh_res["success"])
        self.assertEqual(sh_res["code"], "shell-injection-blocked")

        boot_proc = self._run_cli(
            "--plan", "/boot/limine.conf dosyasını değiştir", "--profile", "zekans-code", "--workspace", str(self.workspace)
        )
        self.assertNotEqual(boot_proc.returncode, 0)
        boot_res = json.loads(boot_proc.stdout)
        self.assertFalse(boot_res["success"])
        self.assertEqual(boot_res["code"], "protected-path-denied")

        # 3. Propose multi-step plan through real zci-runtime + zci-daemon + security
        plan_proc = self._run_cli(
            "--plan",
            "Projeyi analiz et ve onaylı rapor dosyası oluştur",
            "--profile",
            "zekans-code",
            "--workspace",
            str(self.workspace),
        )
        self.assertEqual(plan_proc.returncode, 0, plan_proc.stdout + plan_proc.stderr)
        plan_res = json.loads(plan_proc.stdout)
        self.assertTrue(plan_res["success"])
        plan_d = plan_res["data"]
        plan_id = plan_d["plan_id"]
        self.assertEqual(plan_d["step_count"], 3)
        self.assertEqual(plan_d["approval_required_count"], 1)
        self.assertFalse(plan_d["executable"])
        self.assertEqual(plan_d["policy_authority"], "none")
        self.assertTrue(plan_d["snapper_preflight_checked"])

        # 4. Executing BEFORE approving step-3 MUST FAIL CLOSED (zero writes!)
        unapproved_exec = self._run_cli("--execute", plan_id)
        self.assertNotEqual(unapproved_exec.returncode, 0)
        unapproved_res = json.loads(unapproved_exec.stdout)
        self.assertFalse(unapproved_res["success"])
        self.assertEqual(unapproved_res["code"], "unapproved-steps-pending")
        self.assertFalse(self.target_file.exists())

        # 5. Explicitly approve step-3 -> execute -> succeeds across all 4 real services!
        appr_proc = self._run_cli("--approve", plan_id, "step-3", "--reason", "Stage 13 test onayı")
        self.assertEqual(appr_proc.returncode, 0, appr_proc.stdout + appr_proc.stderr)
        appr_res = json.loads(appr_proc.stdout)
        self.assertTrue(appr_res["success"])
        self.assertEqual(appr_res["data"]["approval_status"], "approved")

        exec_proc = self._run_cli("--execute", plan_id)
        self.assertEqual(exec_proc.returncode, 0, exec_proc.stdout + exec_proc.stderr)
        exec_res = json.loads(exec_proc.stdout)
        self.assertTrue(exec_res["success"], f"Execute failed: {exec_res}")
        self.assertEqual(exec_res["data"]["status"], "completed")
        self.assertEqual(exec_res["data"]["completed_steps"], 3)
        self.assertEqual(exec_res["data"]["denied_steps"], 0)
        self.assertFalse(exec_res["data"]["rollback_execution_enabled"])
        self.assertFalse(exec_res["data"]["repair_execution_enabled"])
        self.assertTrue(self.target_file.is_file())
        self.assertEqual(
            self.target_file.read_text(encoding="utf-8"),
            "Zethropol Stage 13 onaylı ajan çıktısı.\n",
        )

        # 6. Verify immutable hash-chained audit trail & strict 0700/0600 disk permissions
        aud_proc = self._run_cli("--audit", "--limit", "50")
        self.assertEqual(aud_proc.returncode, 0, aud_proc.stdout + aud_proc.stderr)
        aud_res = json.loads(aud_proc.stdout)
        self.assertTrue(aud_res["success"])
        self.assertTrue(aud_res["data"]["chain_valid"])
        self.assertTrue(aud_res["data"]["permissions_strict"])
        self.assertGreaterEqual(aud_res["data"]["entry_count"], 9)

        self.assertEqual(stat.S_IMODE(self.agent_storage.stat().st_mode), 0o700)
        audit_file = self.agent_storage / "audit-trail.jsonl"
        self.assertEqual(stat.S_IMODE(audit_file.stat().st_mode), 0o600)

        # 7. Verify `zci-code-gemma4-shared agent --status` alignment wrapper
        wrapper_proc = subprocess.run(
            [
                "/bin/sh",
                str(HERE / "zci-code-gemma4-shared"),
                "agent",
                "--bus-address",
                self.bus_addr,
                "--status",
            ],
            capture_output=True,
            text=True,
            timeout=15,
            check=False,
        )
        self.assertEqual(wrapper_proc.returncode, 0, wrapper_proc.stderr)
        wrap_json = json.loads(wrapper_proc.stdout)
        self.assertTrue(wrap_json["success"])


if __name__ == "__main__":
    unittest.main()
