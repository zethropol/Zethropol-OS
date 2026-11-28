"""Fail-closed Python client for Zethropol OS ZCI Security & Policy Engine (Stage 08).

Connects user-space ZCI agents (`zci-chat`, `zekans-code`, `zci-command`, `zci-vision`)
to `org.zethropol.Security` (`/org/zethropol/Security/Policy`,
`org.zethropol.Security.Policy.v1`) via `/usr/bin/busctl --json=short`.

Security invariants:
- Zero privilege escalation: never invokes sudo/pkexec or mutates policy state.
- Strict fail-closed: any missing daemon, timeout, transport error, or malformed
  response results in `allowed=False`.
"""
from __future__ import annotations

from dataclasses import dataclass
import json
import os
import subprocess
from typing import Any, Dict, List, Optional

POLICY_BUS_NAME = "org.zethropol.Security"
POLICY_OBJECT_PATH = "/org/zethropol/Security/Policy"
POLICY_INTERFACE = "org.zethropol.Security.Policy.v1"
DEFAULT_BUSCTL_BIN = "/usr/bin/busctl"
DEFAULT_TIMEOUT_SECS = 5.0


@dataclass(frozen=True)
class PolicyEvaluationResult:
    allowed: bool
    decision: str
    action_class: str
    requires_polkit: bool
    polkit_action: str
    rule_id: str
    code: str
    reason: str
    penalty_delta: int
    penalty_score: int
    penalty_level: int
    lockout_active: bool
    hard_locked: bool
    cooldown_remaining_secs: int
    untrusted_flags: List[str]
    audit_event_id: str


@dataclass(frozen=True)
class UntrustedEnvelopeResult:
    ok: bool
    trust_level: str
    executable: bool
    policy_authority: str
    source_kind: str
    content_sha256: str
    injection_detected: bool
    self_modification_injection: bool
    detected_patterns: List[str]
    wrapped_payload: str
    code: str


def _unwrap_busctl_variant(value: Any) -> Any:
    """Recursively unwrap busctl `--json=short` `{type, data}` variant wrappers."""
    if isinstance(value, dict) and set(value.keys()) == {"type", "data"}:
        return _unwrap_busctl_variant(value["data"])
    if isinstance(value, dict):
        return {k: _unwrap_busctl_variant(v) for k, v in value.items()}
    if isinstance(value, list):
        return [_unwrap_busctl_variant(item) for item in value]
    return value


def _fail_closed_decision(code: str, reason: str) -> PolicyEvaluationResult:
    return PolicyEvaluationResult(
        allowed=False,
        decision="deny",
        action_class="SystemStateChange",
        requires_polkit=False,
        polkit_action="",
        rule_id="CLIENT_FAIL_CLOSED",
        code=code,
        reason=reason,
        penalty_delta=0,
        penalty_score=0,
        penalty_level=0,
        lockout_active=False,
        hard_locked=False,
        cooldown_remaining_secs=0,
        untrusted_flags=[],
        audit_event_id="",
    )


class ZciPolicyClient:
    """Fail-closed D-Bus bridge for ZCI Policy Engine (`org.zethropol.Security.Policy.v1`)."""

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

    def _call_method(self, method: str, signature: str, *args: str) -> Dict[str, Any]:
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

        cmd.extend(["call", POLICY_BUS_NAME, POLICY_OBJECT_PATH, POLICY_INTERFACE, method])
        if signature:
            cmd.append(signature)
            cmd.extend(args)

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
            raise RuntimeError(f"busctl call {method} failed (rc={proc.returncode}): {stderr}")

        parsed = json.loads(proc.stdout)
        if not isinstance(parsed, dict) or not isinstance(parsed.get("data"), list) or not parsed["data"]:
            raise ValueError("Malformed busctl JSON reply")
        unwrapped = _unwrap_busctl_variant(parsed["data"][0])
        if not isinstance(unwrapped, dict):
            raise ValueError("Policy reply envelope must be a dictionary")
        return unwrapped

    def evaluate_action(
        self,
        agent_id: str,
        operation: str,
        target: str = "",
        *,
        dbus_interface: str = "",
        untrusted_payload: str = "",
    ) -> PolicyEvaluationResult:
        try:
            reply = self._call_method(
                "EvaluateAction",
                "sssss",
                str(agent_id),
                str(operation),
                str(target),
                str(dbus_interface),
                str(untrusted_payload),
            )
            data = reply.get("data")
            if not isinstance(data, dict):
                return _fail_closed_decision("malformed-policy-reply", "Missing data envelope in policy reply.")
            allowed = reply.get("success") is True and data.get("allowed") is True
            return PolicyEvaluationResult(
                allowed=allowed,
                decision=str(data.get("decision", "deny")),
                action_class=str(data.get("action_class", "SystemStateChange")),
                requires_polkit=bool(data.get("requires_polkit", False)),
                polkit_action=str(data.get("polkit_action", "")),
                rule_id=str(data.get("rule_id", "UNKNOWN_RULE")),
                code=str(reply.get("code", "policy-denied")),
                reason=str(data.get("reason", reply.get("message", "Denied by policy engine."))),
                penalty_delta=int(data.get("penalty_delta", 0)),
                penalty_score=int(data.get("penalty_score", 0)),
                penalty_level=int(data.get("penalty_level", 0)),
                lockout_active=bool(data.get("lockout_active", False)),
                hard_locked=bool(data.get("hard_locked", False)),
                cooldown_remaining_secs=int(data.get("cooldown_remaining_secs", 0)),
                untrusted_flags=[str(x) for x in data.get("untrusted_flags", []) if isinstance(x, str)],
                audit_event_id=str(data.get("audit_event_id", "")),
            )
        except Exception as exc:
            return _fail_closed_decision("policy-engine-unavailable", f"Fail-closed denial: {exc}")

    def wrap_untrusted_data(
        self,
        agent_id: str,
        source_kind: str,
        content: str,
    ) -> UntrustedEnvelopeResult:
        try:
            reply = self._call_method(
                "WrapUntrustedData",
                "sss",
                str(agent_id),
                str(source_kind),
                str(content),
            )
            data = reply.get("data")
            if not isinstance(data, dict) or reply.get("success") is not True:
                raise ValueError("Malformed WrapUntrustedData reply")
            return UntrustedEnvelopeResult(
                ok=True,
                trust_level=str(data.get("trust_level", "untrusted-data-payload")),
                executable=False,
                policy_authority="none",
                source_kind=str(data.get("source_kind", "untrusted_unknown")),
                content_sha256=str(data.get("content_sha256", "")),
                injection_detected=bool(data.get("injection_detected", True)),
                self_modification_injection=bool(data.get("self_modification_injection", False)),
                detected_patterns=[str(x) for x in data.get("detected_patterns", []) if isinstance(x, str)],
                wrapped_payload=str(data.get("wrapped_payload", "")),
                code=str(reply.get("code", "ok")),
            )
        except Exception:
            return UntrustedEnvelopeResult(
                ok=False,
                trust_level="untrusted-data-payload",
                executable=False,
                policy_authority="none",
                source_kind="untrusted_unknown",
                content_sha256="",
                injection_detected=True,
                self_modification_injection=False,
                detected_patterns=["CLIENT_FAIL_CLOSED_UNAVAILABLE"],
                wrapped_payload="",
                code="policy-engine-unavailable",
            )

    def get_policy_summary(self) -> Dict[str, Any]:
        return self._call_method("GetPolicySummary", "")

    def get_agent_status(self, agent_id: str) -> Dict[str, Any]:
        return self._call_method("GetAgentStatus", "s", str(agent_id))

    def get_audit_events(self, limit: int = 50) -> Dict[str, Any]:
        clamped = max(1, min(int(limit), 200))
        return self._call_method("GetAuditEvents", "u", str(clamped))
