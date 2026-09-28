# Zethropol OS — IPC Architecture

## Purpose

This document defines the initial inter-process communication (IPC) architecture for Zethropol OS.

The IPC architecture defines how Zethropol services, user-session components, and other system components communicate through controlled and versionable interfaces.

IPC should provide clear service boundaries while supporting security, reliability, observability, and future architectural flexibility.

## IPC Architecture Principles

- Communication should occur through defined service interfaces.
- IPC should expose stable Zethropol concepts rather than hardware-specific implementation details.
- Read, observe, action, and administrative operations should remain distinguishable.
- Privileged operations must be protected by explicit authorization.
- Interfaces should be versionable and capable of controlled evolution.
- Services should not depend on undocumented private communication channels.
- IPC failures should be detectable and represented through structured errors.
- The IPC mechanism should support both system-level and user-session communication where required.
- Existing Linux IPC and security mechanisms should be preferred over unnecessary custom mechanisms.

## IPC Technology

D-Bus is the validated primary IPC mechanism for Zethropol OS.

D-Bus is widely integrated with Linux and system services and provides mechanisms for service discovery, method calls, return values, errors, signals, service activation, and policy-based access control.

The existing Linux and systemd environment provides substantial D-Bus integration, supporting its use as the primary IPC mechanism for communication between Zethropol services and between user-session components and system services.

The D-Bus prototype has validated D-Bus as the primary IPC mechanism for Zethropol OS. Production bus topology, service registration, activation, authorization, and interface details remain subject to implementation-stage validation.

## IPC Responsibilities

The IPC layer should provide mechanisms for:

- Requesting current service state
- Requesting controlled operations
- Receiving state-change notifications
- Reporting structured errors
- Discovering available service interfaces
- Determining service availability
- Supporting authorized privileged operations

The IPC mechanism should transport service information but should not become the owner of service logic or system policy.

## System and Session Buses

Zethropol should distinguish between system-level and user-session communication.

System-level services should use the appropriate system IPC context for operations affecting system-wide state.

User-session components should use the appropriate session IPC context for user-specific functionality.

Communication between a user-session component and a privileged system service should cross an explicit service boundary and be subject to the authorization policy of the receiving service.

The exact bus topology and service registration model will be defined during IPC prototyping.

## Service Interface Model

Each Zethropol IPC service should expose a defined interface representing its responsibilities.

An interface should provide only the operations required by its consumers.

The initial operation categories are:

- READ — retrieve current state without modifying system state
- OBSERVE — receive notifications about relevant state changes
- ACTION — perform a controlled non-administrative operation
- ADMIN — perform a privileged or system-wide operation requiring explicit authorization

These categories are defined by the API architecture and should remain visible at the IPC interface level.

## Methods

IPC methods should represent explicit operations with defined inputs, outputs, and error conditions.

Methods should use structured data rather than relying on human-readable strings as their primary data format.

Methods that modify system state should define their authorization requirements and expected state transitions.

Long-running operations should not require clients to maintain an uncontrolled synchronous connection where asynchronous operation reporting is more appropriate.

## Signals and Observation

Services should use event or signal mechanisms for state changes that clients need to observe.

Observation should avoid requiring clients to repeatedly poll a service when the underlying state supports reliable event notification.

Examples may include:

- Hardware availability changes
- Thermal state changes
- Power state changes
- Performance profile changes
- Configuration changes
- Service state changes

Signals should contain sufficient structured information for consumers to understand the event without exposing unnecessary implementation details.

## Data Model

IPC data should use stable Zethropol data models.

Hardware-specific paths, device nodes, kernel implementation details, and internal service state should not normally cross the public service interface.

For example, a monitoring consumer should request normalized GPU utilization rather than a specific sysfs or driver-specific path.

Data models should distinguish between:

- Available values
- Unavailable values
- Unsupported capabilities
- Temporarily inaccessible values
- Invalid or failed state

Clients must not interpret unavailable data as a valid zero or default value.

## Error Model

IPC errors should use the structured error model defined by the Zethropol API architecture.

Errors should distinguish between relevant conditions such as:

- Invalid request
- Unsupported operation
- Missing capability
- Authorization failure
- Service unavailable
- Dependency unavailable
- Resource unavailable
- Operation failure
- Timeout or communication failure

Error information should provide useful diagnostic context without unnecessarily exposing sensitive implementation details.

## Authorization and Security

IPC must not be treated as a trusted boundary merely because communication occurs locally.

Services must evaluate authorization for privileged operations.

User interfaces must not be granted unrestricted access to privileged service methods.

Authorization should consider the requesting identity, requested operation, and applicable security policy.

Zethropol will use established Linux authorization mechanisms for privileged service operations. Production services should use polkit for service-side authorization decisions, with service-specific action identifiers and policies.

D-Bus policy will control service ownership and IPC access but will not be treated as a replacement for operation-level authorization. The receiving service remains responsible for enforcing authorization before performing privileged operations.

## Service Discovery and Availability

Clients should be able to determine whether a required service is available.

Service availability should be distinguishable from service readiness where practical.

A registered service may exist while still initializing or while one of its required capabilities is unavailable.

Clients should handle service disappearance and reappearance without assuming that a connection remains permanently valid.

## Service Activation

Where appropriate, services may be activated on demand rather than requiring every service to remain continuously active.

Service activation should integrate with systemd and D-Bus mechanisms where practical.

Activation decisions should consider startup cost, service criticality, expected usage, and dependency requirements.

The final activation model will be determined during process and IPC prototyping.

## Timeouts and Failure Handling

IPC clients should not assume that every request completes immediately.

Operations should have bounded request deadlines appropriate to their operation type.

A D-Bus request timeout applies to communication and request-response handling. It must not automatically imply that an underlying operation failed or was cancelled.

Synchronous operations should complete within an operation-appropriate bounded deadline.

Asynchronous operations should use a bounded acceptance deadline. Once accepted, execution is tracked independently through the asynchronous operation model.

Timeouts must not be treated as implicit cancellation.

Long-running operations should therefore use the established asynchronous mechanism rather than extending a synchronous D-Bus call indefinitely.

Services should remain usable when unrelated IPC services are unavailable.

### Retry Safety

A timeout or communication failure does not establish whether the requested operation was executed.

Clients must not automatically retry an operation unless its retry behavior is explicitly defined by the service contract.

The initial retry-safety rules are:

- READ operations should be safe to retry when they are defined as idempotent.
- OBSERVE operations should be re-established according to the observation contract rather than treated as repeated state-changing requests.
- ACTION and ADMIN operations must not be automatically retried unless the service contract explicitly defines them as retry-safe.
- When an operation is not retry-safe, the client should obtain current service or operation state before deciding whether another request is necessary.
- An asynchronous operation that has already been accepted must be tracked through its `operationId`; retrying the original operation must not be used as a substitute for operation tracking.
- If an acceptance response is lost before the client obtains an `operationId`, the client must treat the outcome as unknown unless the operation provides an explicit request identity mechanism that makes retry safe.

Services may support request identity or idempotency keys for operations where reliable retry is required. Such mechanisms must define their scope, lifetime, duplicate-request behavior, and relationship to the resulting `operationId`.

Service-specific timeout values and retry behavior are implementation-stage properties and must be documented by each production service contract.

## Compatibility and Versioning

Zethropol IPC interfaces should be designed for controlled evolution.

Interfaces should have explicit versions or another clearly defined compatibility mechanism.

Backward-compatible additions should be preferred where practical.

Breaking interface changes should require an explicit version transition rather than silently changing the meaning of an existing operation.

Clients should be able to determine which interface version and capabilities a service provides.

## Naming and Interface Organization

Zethropol IPC naming should follow a consistent namespace and object organization.

Names should represent stable Zethropol concepts rather than implementation-specific process names or hardware paths.

The baseline D-Bus service identity naming convention is established by the validated prototype.

- Bus name: `org.zethropol.<Service>`
- Object path: `/org/zethropol/<Service>`
- Primary interface: `org.zethropol.<Service>.v1`

Service-specific methods, properties, signals, additional interfaces, and production service names remain subject to the logical service model.

## Privileged Operations

Privileged operations should be exposed only by services responsible for the affected system resource.

A user-facing component should request a privileged operation through the appropriate service rather than attempting direct system access.

The receiving service should validate the request and perform authorization before applying the operation.

Administrative operations should provide explicit success or structured failure information.

## Configuration Communication

Configuration changes should use the Configuration Service interface where the configuration is owned by Zethropol.

Clients should not directly modify configuration files owned by another service.

Configuration requests should be validated before being applied.

Where configuration changes affect running services, the relevant service should define whether the change is applied immediately, reloaded, or requires restart.

## Hardware and IPC

Hardware-specific implementation details should remain below the Hardware Service boundary.

Other services should consume normalized hardware information through the Hardware Service interface.

IPC consumers should therefore remain independent from specific hardware vendors, drivers, sysfs paths, procfs paths, hwmon layouts, and device naming conventions.

## Observability

IPC activity should be observable through appropriate diagnostic and logging mechanisms.

Where practical, observability should allow developers and administrators to determine:

- Which service handled a request
- Whether a request succeeded or failed
- Which authorization decision was applied
- Whether a service was unavailable
- Whether communication timed out
- Whether a service repeatedly disconnected or restarted

Observability should avoid exposing sensitive data unnecessarily.

### Logging and Audit

Zethropol services should use established Linux and systemd logging facilities, including the systemd journal, rather than introducing a separate Zethropol-wide logging daemon.

Diagnostic logging should provide sufficient context for service lifecycle, readiness, IPC failures, operation failures, timeouts, and unexpected service restarts.

Audit logging should be considered for privileged operations, authorization decisions and failures, and significant security-state changes.

Logs and audit records must avoid unnecessary exposure of sensitive data.

Service-specific log detail, event selection, retention, and other implementation requirements remain the responsibility of the applicable production service contract.

## Testing

IPC interfaces should be independently testable.

Testing should cover:

- Successful method calls
- Invalid requests
- Structured errors
- Authorization failures
- Service unavailability
- Service restart
- Service disappearance and reappearance
- Signals and event delivery
- Timeout behavior
- Interface compatibility
- Multiple simultaneous clients

IPC tests should not require a complete graphical desktop environment.

## Relationship with Process Architecture

IPC boundaries should correspond to actual communication requirements between processes or logical service components.

The process architecture defines how components may execute, while the IPC architecture defines how those components communicate.

Neither architecture requires every logical service to become an independent process.

If multiple logical services share a process, their internal communication may use direct interfaces rather than IPC while preserving the same logical service boundaries.

## Relationship with Existing System Components

Zethropol should build upon D-Bus, systemd, Linux authorization mechanisms, and other established IPC facilities provided by the CachyOS/Arch base system.

Zethropol should not replace established IPC infrastructure without a clear architectural reason.

KDE Plasma may consume Zethropol service interfaces, but core Zethropol services should not depend on Plasma-specific IPC mechanisms.

## Current Architectural Status

The following IPC principles are established:

- Defined service interfaces
- Stable Zethropol data models
- Distinction between READ, OBSERVE, ACTION, and ADMIN operations
- Explicit authorization for privileged operations
- Structured IPC errors
- System and user-session communication separation
- Service availability and failure handling
- Interface versioning
- Hardware abstraction through the Hardware Service boundary
- Integration with Linux and systemd infrastructure

The following decisions remain open:

- Production D-Bus bus topology and service registration details
- Service-specific object, interface, method, property, and signal definitions
- Service-specific polkit action definitions and authorization policy
- Service activation model
- Service-specific timeout values and retry-safe semantics
- Interface versioning implementation

These decisions will be refined through production service implementation and IPC integration.

## IPC Architecture Status

This document represents the initial IPC architecture for Stage 2.

It is a working architectural document and will evolve as the Zethropol communication model is validated.
