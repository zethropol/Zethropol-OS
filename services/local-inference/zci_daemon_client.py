"""Fail-closed Python client for Zethropol OS ZCI Daemon (Stage 10).

Connects user-space ZCI agents (`zci-chat`, `zekans-code`, `zci-command`, `zci-vision`)
to `org.zethropol.ZciDaemon` (`/org/zethropol/ZciDaemon`, `org.zethropol.ZciDaemon.v1`)
via `/usr/bin/busctl --json=short`.

Security invariants:
- Zero privilege escalation: never invokes sudo/pkexec.
- Strict fail-closed: any missing daemon, timeout, transport error, or malformed
  response yields `success=False` (`code="zci-daemon-unavailable"`).
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
import subprocess
from typing import Any, Dict, Optional

DAEMON_BUS_NAME = "org.zethropol.ZciDaemon"
DAEMON_OBJECT_PATH = "/org/zethropol/ZciDaemon"
DAEMON_INTERFACE = "org.zethropol.ZciDaemon.v1"
DEFAULT_BUSCTL_BIN = "/usr/bin/busctl"
DEFAULT_TIMEOUT_SECS = 10.0


@dataclass(frozen=True)
class GatewayActionResult:
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


def _fail_closed(code: str, message: str) -> GatewayActionResult:
    return GatewayActionResult(
        success=False,
        code=code,
        message=message,
        operation_id="",
        data={},
    )


class ZciDaemonClient:
    """Fail-closed D-Bus bridge for ZCI Daemon (`org.zethropol.ZciDaemon.v1`)."""

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
        if timeout_secs <= 0 or timeout_secs > 30:
            raise ValueError("timeout_secs must be in (0, 30]")
        self.busctl_bin = busctl_bin
        self.bus_address = bus_address
        self.use_user_bus = use_user_bus
        self.timeout_secs = timeout_secs

    def _call_method(self, method: str, signature: str, *args: str) -> GatewayActionResult:
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

        cmd.extend(["call", DAEMON_BUS_NAME, DAEMON_OBJECT_PATH, DAEMON_INTERFACE, method])
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
                    "zci-daemon-unavailable",
                    f"busctl call {method} failed (rc={proc.returncode}): {stderr}",
                )
            parsed = json.loads(proc.stdout)
            if not isinstance(parsed, dict) or not isinstance(parsed.get("data"), list) or not parsed["data"]:
                return _fail_closed("malformed-daemon-reply", "Malformed busctl JSON reply.")
            unwrapped = _unwrap_busctl_variant(parsed["data"][0])
            if not isinstance(unwrapped, dict) or not isinstance(unwrapped.get("data"), dict):
                return _fail_closed("malformed-daemon-reply", "Daemon reply envelope missing data map.")
            return GatewayActionResult(
                success=unwrapped.get("success") is True,
                code=str(unwrapped.get("code", "unknown")),
                message=str(unwrapped.get("message", "")),
                operation_id=str(unwrapped.get("operationId", "")),
                data=dict(unwrapped["data"]),
            )
        except Exception as exc:
            return _fail_closed("zci-daemon-unavailable", f"Fail-closed denial: {exc}")

    def open_session(self, agent_id: str) -> GatewayActionResult:
        return self._call_method("OpenSession", "s", str(agent_id))

    def set_session_standby(self, session_id: str) -> GatewayActionResult:
        return self._call_method("SetSessionStandby", "s", str(session_id))

    def close_session(self, session_id: str) -> GatewayActionResult:
        return self._call_method("CloseSession", "s", str(session_id))

    def execute_action(
        self,
        session_id: str,
        agent_id: str,
        action_kind: str,
        operation: str,
        target: str = "",
        *,
        dbus_interface: str = "",
        payload: str = "",
    ) -> GatewayActionResult:
        return self._call_method(
            "ExecuteAction",
            "sssssss",
            str(session_id),
            str(agent_id),
            str(action_kind),
            str(operation),
            str(target),
            str(dbus_interface),
            str(payload),
        )

    def wrap_untrusted_content(
        self, agent_id: str, source_kind: str, content: str
    ) -> GatewayActionResult:
        return self._call_method(
            "WrapUntrustedContent", "sss", str(agent_id), str(source_kind), str(content)
        )

    def get_daemon_status(self) -> GatewayActionResult:
        return self._call_method("GetDaemonStatus", "")

    def list_sessions(self) -> GatewayActionResult:
        return self._call_method("ListSessions", "")

    def terminate_session(self, session_id: str, reason: str = "") -> GatewayActionResult:
        return self._call_method("TerminateSession", "ss", str(session_id), str(reason))

    def reload_config(self, reason: str = "") -> GatewayActionResult:
        return self._call_method("ReloadConfig", "s", str(reason))
