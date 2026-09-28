# Zethropol D-Bus Architecture Checkpoint

## Status

The D-Bus architecture prototype has been validated on both the user session bus and the system bus.
This document records validated architectural evidence. It does not define the final production service API.

## Validated Architecture

- System D-Bus broker: dbus-broker
- IPC transport: D-Bus
- Service activation: D-Bus activation through systemd
- Service type: systemd Type=dbus
- Qt implementation: Qt 6 QDBus
- System bus name used by the prototype: org.zethropol.SystemPrototype
- Object path used by the prototype: /org/zethropol/Prototype
- Interface used by the prototype: org.zethropol.Service

## Validated Operations

- READ: serviceStatus()
- READ: serviceInfo()
- ACTION: performAction(QString)
- ADMIN: performAdminAction(QString)
- OBSERVE: statusChanged(QString)

## Validated Security Boundary

The prototype demonstrates that privileged system-facing operations can be placed behind a system-bus service boundary rather than being executed directly by the desktop UI.

ADMIN operations are explicitly separated from normal ACTION operations.
The prototype currently returns authorization-required for the ADMIN operation.
This validates the architectural boundary but does not yet define the final authorization mechanism.

## Validated Activation Model

1. A D-Bus client requests the service name.
2. The system D-Bus broker resolves the activation definition.
3. systemd starts the corresponding service unit.
4. The service acquires its D-Bus name.
5. The client communicates through the declared object path and interface.

## Production Decisions Established

- Production service identity convention
- Initial production service contracts
- Initial READ / OBSERVE / ACTION / ADMIN member model
- Initial structured data schemas
- Structured result and error model
- D-Bus structured transport encoding
- Initial D-Bus method and signal naming convention
- Initial service readiness and availability model
- Initial asynchronous operation model
- Initial D-Bus timeout semantics
- Initial service lifecycle and failure-recovery policy

## Production Decisions Still Open

- Service-specific timeout values, retry limits, and backoff behavior
- Service-specific request identity or idempotency mechanisms where required
- Service-specific polkit action definitions and authorization policy
- Service-specific logging detail, event selection, retention, and audit requirements
- Service-specific sandboxing and systemd hardening profiles
- Final compatibility and interface-versioning mechanism

## Architectural Rule

Zethropol desktop components must not directly perform privileged system operations when a dedicated Zethropol service boundary is responsible for that operation.

The final production service architecture must remain hardware-independent and must expose normalized Zethropol data rather than machine-specific implementation details.

## Prototype Commits

- c8a4940 Add D-Bus service architecture prototype
- 26bbba6 Add system D-Bus service prototype
