"""Fail-closed Python client for Zethropol OS ZCI Runtime Engine (Stage 11).

Connects user-space ZCI agents (`zci-chat`, `zekans-code`, `zci-command`, `zci-vision`)
to `org.zethropol.ZciRuntime` (`/org/zethropol/ZciRuntime`, `org.zethropol.ZciRuntime.v1`)
via `/usr/bin/busctl --json=short`.

Security invariants:
- Zero privilege escalation: never invokes sudo/pkexec.
- Strict fail-closed: any unreachable runtime, timeout, transport error, or malformed
  response yields `success=False` (`code="zci-runtime-unavailable"`).
- Strict untrusted output enforcement: `run_inference` validates that returned payloads
  carry `output_executable=False` and `output_policy_authority="none"`.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
import subprocess
from typing import Any, Dict, Optional

RUNTIME_BUS_NAME = "org.zethropol.ZciRuntime"
RUNTIME_OBJECT_PATH = "/org/zethropol/ZciRuntime"
RUNTIME_INTERFACE = "org.zethropol.ZciRuntime.v1"
DEFAULT_BUSCTL_BIN = "/usr/bin/busctl"
DEFAULT_TIMEOUT_SECS = 15.0


@dataclass(frozen=True)
class RuntimeCallResult:
    success: bool
    code: str
    message: str
    operation_id: str
    data: Dict[str, Any]


def _unwrap_busctl_variant(value: Any) -> Any:
    if isinstance(value, dict) and set(value.keys()) == {"type", "data"}:
        return _unwrap_busctl_variant(value["data"])
    if isinstance(value, dict):
        return {k: _unwrap_busctl_variant(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_unwrap_busctl_variant(item) for item in value]
    return value


def _fail_closed(code: str, message: str) -> RuntimeCallResult:
    return RuntimeCallResult(
        success=False,
        code=code,
        message=message,
        operation_id="",
        data={},
    )


class ZciRuntimeClient:
    """Fail-closed D-Bus bridge for ZCI Runtime Engine (`org.zethropol.ZciRuntime.v1`)."""

    def __init__(
        self,
        *,
        busctl_bin: str = DEFAULT_BUSCTL_BIN,
        bus_address: Optional[str] = None,
        use_user_bus: bool = False,
        timeout_secs: float = DEFAULT_TIMEOUT_SECS,
    ) -> None:
        if not busctl_bin or not os.path.isabs(busctl_bin):
            raise ValueError("busctl_bin must be an absolute executable path")
        if timeout_secs <= 0 or timeout_secs > 60:
            raise ValueError("timeout_secs must be in (0, 60]")
        self.busctl_bin = busctl_bin
        self.bus_address = bus_address
        self.use_user_bus = use_user_bus
        self.timeout_secs = timeout_secs

    def _call_method(self, method: str, signature: str, *args: str) -> RuntimeCallResult:
        cmd = [self.busctl_bin, "--json=short"]
        env = {"LC_ALL": "C", "PATH": "/usr/bin:/bin"}
        if self.bus_address:
            cmd.extend(["--address", self.bus_address])
        elif self.use_user_bus:
            cmd.append("--user")
            if "DBUS_SESSION_BUS_ADDRESS" in os.environ:
                env["DBUS_SESSION_BUS_ADDRESS"] = os.environ["DBUS_SESSION_BUS_ADDRESS"]
            if "XDG_RUNTIME_DIR" in os.environ:
                env["XDG_RUNTIME_DIR"] = os.environ["XDG_RUNTIME_DIR"]
        else:
            cmd.append("--system")

        cmd.extend(["call", RUNTIME_BUS_NAME, RUNTIME_OBJECT_PATH, RUNTIME_INTERFACE, method])
        if signature:
            cmd.append(signature)
            cmd.extend(args)

        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=self.timeout_secs,
                env=env,
                check=False,
            )
            if proc.returncode != 0:
                stderr = (proc.stderr or "").strip()
                return _fail_closed(
                    "zci-runtime-unavailable",
                    f"busctl call {method} failed (rc={proc.returncode}): {stderr}",
                )
            parsed = json.loads(proc.stdout)
            if not isinstance(parsed, dict) or not isinstance(parsed.get("data"), list) or not parsed["data"]:
                return _fail_closed("malformed-runtime-reply", "Malformed busctl JSON reply.")
            unwrapped = _unwrap_busctl_variant(parsed["data"][0])
            if not isinstance(unwrapped, dict) or not isinstance(unwrapped.get("data"), dict):
                return _fail_closed("malformed-runtime-reply", "Runtime reply envelope missing data map.")
            return RuntimeCallResult(
                success=unwrapped.get("success") is True,
                code=str(unwrapped.get("code", "unknown")),
                message=str(unwrapped.get("message", "")),
                operation_id=str(unwrapped.get("operationId", "")),
                data=dict(unwrapped["data"]),
            )
        except Exception as exc:
            return _fail_closed("zci-runtime-unavailable", f"Fail-closed denial: {exc}")

    def get_runtime_status(self) -> RuntimeCallResult:
        return self._call_method("GetRuntimeStatus", "")

    def get_hardware_profile(self) -> RuntimeCallResult:
        return self._call_method("GetHardwareProfile", "")

    def list_models(self) -> RuntimeCallResult:
        return self._call_method("ListModels", "")

    def verify_model_integrity(self, model_id: str) -> RuntimeCallResult:
        return self._call_method("VerifyModelIntegrity", "s", str(model_id))

    def load_model(self, model_id: str) -> RuntimeCallResult:
        return self._call_method("LoadModel", "s", str(model_id))

    def unload_model(self, model_id: str, reason: str = "") -> RuntimeCallResult:
        return self._call_method("UnloadModel", "ss", str(model_id), str(reason))

    def enter_standby(self, reason: str = "") -> RuntimeCallResult:
        return self._call_method("EnterStandby", "s", str(reason))

    def reload_manifest(self, reason: str = "") -> RuntimeCallResult:
        return self._call_method("ReloadManifest", "s", str(reason))

    def run_inference(
        self,
        agent_id: str,
        model_id: str,
        prompt: str,
        *,
        max_tokens: int = 256,
    ) -> RuntimeCallResult:
        if type(max_tokens) is not int or max_tokens < 0 or max_tokens > 32768:
            return _fail_closed("invalid-max-tokens", "max_tokens must be an integer in [0, 32768].")
        res = self._call_method(
            "RunInference", "sssu", str(agent_id), str(model_id), str(prompt), str(max_tokens)
        )
        # Fail-closed verification that non-standby completions are sealed untrusted data.
        if res.success and res.code == "ok":
            if res.data.get("output_executable") is not False or res.data.get("output_policy_authority") != "none":
                return _fail_closed(
                    "output-isolation-violation",
                    "Fail-closed client drop: runtime output lacked non-executable isolation metadata.",
                )
        return res
