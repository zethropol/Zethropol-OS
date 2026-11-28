"""Unit and real 3-service private D-Bus integration tests for ZciRuntimeClient (Stage 11).

Spawns ALL THREE real Rust binaries on an isolated private `dbus-daemon`:
  1. `services/security` (`org.zethropol.Security.Policy.v1`)
  2. `services/zci-daemon` (`org.zethropol.ZciDaemon.v1`)
  3. `services/zci-runtime` (`org.zethropol.ZciRuntime.v1`)
Plus a local `127.0.0.1` loopback mock inference HTTP backend to verify end-to-end
hardware profile detection, manifest SHA-256 verification, Polkit boundaries,
passive standby lifecycle, bidirectional untrusted sealing, and fail-closed rejections.
"""
from __future__ import annotations

import hashlib
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import os
from pathlib import Path
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


class _MockLoopbackInferenceHandler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:
        length = int(self.headers.get("Content-Length", "0"))
        _ = self.rfile.read(length)
        payload = json.dumps(
            {
                "choices": [
                    {
                        "message": {
                            "content": "Zethropol Stage 11 loopback cevap (Gemma 4B pasif hibrit)."
                        }
                    }
                ]
            }
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format: str, *args: object) -> None:
        return


class ZciRuntimeClientFailClosedTests(unittest.TestCase):
    def test_fail_closed_when_runtime_unreachable(self) -> None:
        client = ZciRuntimeClient(
            bus_address="unix:path=/tmp/nonexistent-zethropol-zci11.sock",
            timeout_secs=2.0,
        )
        res = client.run_inference("zci-chat", "gemma-4-E4B-it-Q4_K_M", "Merhaba")
        self.assertFalse(res.success)
        self.assertEqual(res.code, "zci-runtime-unavailable")

    def test_rejects_relative_busctl_and_bad_tokens(self) -> None:
        with self.assertRaises(ValueError):
            ZciRuntimeClient(busctl_bin="busctl")
        client = ZciRuntimeClient(
            bus_address="unix:path=/tmp/nonexistent-zethropol-zci11.sock",
            timeout_secs=2.0,
        )
        bad = client.run_inference("zci-chat", "m", "hi", max_tokens=999999)
        self.assertFalse(bad.success)
        self.assertEqual(bad.code, "invalid-max-tokens")


class ZciRuntimeThreeServiceIntegrationTests(unittest.TestCase):
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
        if not (self.sec_bin and self.daemon_bin and self.runtime_bin):
            self.skipTest("Compiled security, zci-daemon, and zci-runtime binaries required")

        self.tmp = tempfile.TemporaryDirectory(prefix="zci-runtime-e2e-")
        self.root = Path(self.tmp.name)
        self.fake_home = self.root / "home"
        self.fake_home.mkdir(parents=True)

        # Create real verified GGUF file + tampered GGUF file
        good_gguf = self.root / "gemma-4-E4B-it-Q4_K_M.gguf"
        good_bytes = b"GGUF\x03\x00\x00\x00zethropol-stage11-verified-weights"
        good_gguf.write_bytes(good_bytes)
        good_sha = hashlib.sha256(good_bytes).hexdigest()

        bad_gguf = self.root / "tampered.gguf"
        bad_bytes = b"GGUF\x03\x00\x00\x00tampered-weights"
        bad_gguf.write_bytes(bad_bytes)

        manifest_file = self.root / "zci-models-manifest.json"
        manifest_file.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "models": [
                        {
                            "model_id": "gemma-4-E4B-it-Q4_K_M",
                            "display_name": "Gemma 4 E4B Instruct",
                            "format": "gguf",
                            "path": str(good_gguf),
                            "size_bytes": len(good_bytes),
                            "sha256": good_sha,
                            "license": "Apache-2.0",
                            "min_profile": "dev-baseline",
                            "required_ram_mb": 4096,
                            "required_vram_mb": 0,
                            "hybrid_vram_estimate_mb": 1536,
                            "max_context_tokens": 8192,
                            "trusted": True,
                            "signature_status": "verified-manifest-pin",
                        },
                        {
                            "model_id": "tampered-model",
                            "display_name": "Tampered Model",
                            "format": "gguf",
                            "path": str(bad_gguf),
                            "size_bytes": len(bad_bytes),
                            "sha256": "0" * 64,
                            "license": "Apache-2.0",
                            "min_profile": "dev-baseline",
                            "required_ram_mb": 4096,
                            "required_vram_mb": 0,
                            "hybrid_vram_estimate_mb": 1536,
                            "max_context_tokens": 8192,
                            "trusted": True,
                            "signature_status": "verified-manifest-pin",
                        },
                    ],
                }
            ),
            encoding="utf-8",
        )

        self.pk_allow_flag = self.root / "pk-allow"
        self.mock_pkcheck = self.root / "mock-pkcheck.sh"
        self.mock_pkcheck.write_text(
            f'#!/usr/bin/env bash\nif [ -f "{self.pk_allow_flag}" ]; then exit 0; else exit 1; fi\n',
            encoding="utf-8",
        )
        self.mock_pkcheck.chmod(0o755)

        # Start loopback mock inference HTTP server
        self.httpd = HTTPServer(("127.0.0.1", 0), _MockLoopbackInferenceHandler)
        self.engine_port = self.httpd.server_address[1]
        self.http_thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.http_thread.start()

        # Start isolated private dbus-daemon
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
                "--config-file",
                str(self.root / "zci-runtime.json"),
                "--manifest-file",
                str(manifest_file),
                "--engine-endpoint",
                f"127.0.0.1:{self.engine_port}",
                "--idle-sleep-secs",
                "1",
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        self.client = ZciRuntimeClient(bus_address=self.bus_addr, timeout_secs=5.0)
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            st = self.client.get_runtime_status()
            if (
                st.success
                and st.data.get("policy_engine_reachable") is True
                and st.data.get("daemon_bridge_reachable") is True
            ):
                return
            time.sleep(0.05)
        self.fail("Real security + zci-daemon + zci-runtime did not become ready on private D-Bus")

    def tearDown(self) -> None:
        if hasattr(self, "httpd"):
            self.httpd.shutdown()
            self.httpd.server_close()
        if getattr(self, "dbus_proc", None) is not None and self.dbus_proc.stdout is not None:
            self.dbus_proc.stdout.close()
        for proc in (
            getattr(self, "runtime_proc", None),
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

    def test_three_service_real_dbus_runtime_pipeline(self) -> None:
        # 1. Verify status, hardware profile & loopback-only posture
        st = self.client.get_runtime_status()
        self.assertTrue(st.success)
        self.assertEqual(st.code, "ok")
        self.assertTrue(st.data["loopback_only"])
        self.assertTrue(st.data["external_network_disabled"])
        self.assertTrue(st.data["policy_engine_reachable"])
        self.assertTrue(st.data["daemon_bridge_reachable"])
        self.assertEqual(st.data["runtime_state"], "Standby")
        self.assertFalse(st.data["vram_locked"])
        self.assertEqual(st.data["active_vram_mb"], 0)
        self.assertIn(st.data["hardware_profile"], ("dev-baseline", "ancha-production"))
        self.assertEqual(st.data["verified_model_count"], 1)
        self.assertEqual(st.data["rejected_model_count"], 1)

        # 2. Polkit blocks LoadModel before admin authorization
        deny_load = self.client.load_model("gemma-4-E4B-it-Q4_K_M")
        self.assertFalse(deny_load.success)
        self.assertEqual(deny_load.code, "permission-denied")

        # 3. Grant Polkit -> tampered SHA-256 model STILL rejected fail-closed!
        self.pk_allow_flag.write_text("allow", encoding="utf-8")
        bad_load = self.client.load_model("tampered-model")
        self.assertFalse(bad_load.success)
        self.assertEqual(bad_load.code, "sha256-mismatch")

        # 4. Load verified Gemma 4B model into passive Standby
        ok_load = self.client.load_model("gemma-4-E4B-it-Q4_K_M")
        self.assertTrue(ok_load.success)
        self.assertEqual(ok_load.data["runtime_state"], "Standby")
        self.assertFalse(ok_load.data["vram_locked"])

        # 5. Run end-to-end loopback inference through REAL Security + REAL ZCI Daemon + REAL ZCI Runtime!
        inf = self.client.run_inference(
            "zci-chat",
            "gemma-4-E4B-it-Q4_K_M",
            "Merhaba Zethropol, sistem durumunu özetle.",
            max_tokens=128,
        )
        self.assertTrue(inf.success, inf.message)
        self.assertEqual(inf.code, "ok")
        self.assertFalse(inf.data["input_executable"])
        self.assertEqual(inf.data["input_policy_authority"], "none")
        self.assertFalse(inf.data["output_executable"])
        self.assertEqual(inf.data["output_policy_authority"], "none")
        self.assertIn("Zethropol Stage 11 loopback cevap", inf.data["output_wrapped_payload"])

        # 6. Wait for 1s auto-sleep passive standby transition (background sweeper!)
        deadline = time.monotonic() + 4.0
        slept = False
        while time.monotonic() < deadline:
            cur = self.client.get_runtime_status()
            if (
                cur.success
                and cur.data.get("runtime_state") == "Standby"
                and cur.data.get("vram_locked") is False
                and cur.data.get("auto_sleep_transitions_total", 0) >= 1
            ):
                slept = True
                break
            time.sleep(0.15)
        self.assertTrue(slept, "Expected automatic Active -> Standby transition after idle timeout")

        # 7. Unload model cleanly
        unl = self.client.unload_model("gemma-4-E4B-it-Q4_K_M", "test-finished")
        self.assertTrue(unl.success)


if __name__ == "__main__":
    unittest.main()
