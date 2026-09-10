# Zethropol OS — Process Architecture

## Purpose

This document defines the initial process and execution architecture for Zethropol OS.

The process architecture establishes how Zethropol services and supporting components may be executed, isolated, managed, and integrated with the underlying Linux and systemd environment.

Process boundaries should support security, reliability, testability, observability, and maintainability without creating unnecessary process complexity.

## Process Architecture Principles

- Process boundaries should follow clear responsibility and security requirements.
- A logical service boundary does not automatically require a separate process.
- Privileged functionality should be isolated from unprivileged user-facing components.
- Long-running system services should use established system management mechanisms where appropriate.
- User-session components should remain separate from system-wide privileged services.
- Services should fail independently where practical.
- Process communication should use defined service interfaces rather than private implementation channels.
- Process architecture should remain independent from the visual desktop interface.
- Existing Linux and systemd mechanisms should be reused rather than unnecessarily replaced.

## Logical Service and Process Boundaries

Zethropol services are currently defined as logical architectural responsibilities.

These logical services may eventually be implemented as:

- Independent system processes
- Independent user-session processes
- Multiple cooperating processes
- Components embedded within another Zethropol service where isolation is not required

The final process layout should be determined by responsibility, privilege, lifecycle, failure isolation, resource usage, and communication requirements.

Logical service boundaries must remain clear even when multiple services temporarily share a process.

## System Services

Services responsible for system-wide hardware, monitoring, performance, power, diagnostics, updates, recovery, or privileged configuration may require system-level execution.

System-level services should operate under controlled privileges and should not depend on a graphical desktop session.

Where systemd is used, service lifecycle, startup, restart behavior, dependency handling, and resource controls should integrate with systemd mechanisms.

System services should expose only the interfaces required by their responsibilities.

## User-Session Services

User-specific functionality may run within the user session rather than as a system-wide privileged process.

User-session services may provide user-specific configuration access, notifications, desktop integration, or other functions that do not require system privileges.

User-session processes must not gain administrative privileges merely because they are part of the Zethropol user experience.

User-session services should communicate with system services through defined interfaces when privileged or system-wide functionality is required.

## Privilege Separation

Process boundaries should reinforce the Zethropol privilege model.

The initial logical model is:

User Interface
        ↓
User-Session Components
        ↓
Zethropol Service Interface
        ↓
Privileged or System Services
        ↓
Linux / systemd / hardware interfaces

User-facing processes should not directly execute privileged system operations when a controlled service boundary is available.

Privileged services should perform authorization checks independently of the requesting user interface.

## Service Lifecycle

Each long-running Zethropol service should have a defined lifecycle.

The lifecycle should account for:

- Startup
- Initialization
- Dependency availability
- Ready state
- Normal operation
- Configuration changes
- Temporary failure
- Restart
- Shutdown

Services should not assume that all dependencies are permanently available.

Where systemd manages a service, systemd dependency and readiness mechanisms should be evaluated rather than implementing unnecessary custom lifecycle management.

## Startup and Dependency Management

Process startup order should be based on actual runtime requirements rather than the logical dependency diagram alone.

A service should start when its required dependencies are available or should handle dependency unavailability explicitly.

Service dependencies should avoid unnecessary ordering constraints.

The dependency architecture defined in SERVICE_DEPENDENCIES.md represents logical relationships and does not define mandatory process startup order.

## Failure Isolation

Process boundaries should provide useful failure isolation where practical.

A failure in a non-critical service should not unnecessarily terminate unrelated services or the desktop session.

Critical services should have clearly defined failure behavior.

Where a service can be safely restarted, the process architecture should allow controlled restart without requiring a complete system restart.

Failure isolation must not be achieved by creating unnecessary processes when the resulting complexity provides no meaningful reliability or security benefit.

## Resource Management

Long-running Zethropol processes should have controlled resource behavior.

Resource management may include:

- CPU usage limits or scheduling policy where appropriate
- Memory limits where appropriate
- I/O resource controls where appropriate
- Process priority
- Restart limits
- Logging limits

Resource controls should be applied according to service responsibility and actual requirements.

Zethropol should reuse systemd and Linux resource-control mechanisms where appropriate.

## IPC and Process Communication

Processes should communicate through defined Zethropol service interfaces.

D-Bus is the primary IPC candidate established by the current architecture, but the final IPC mechanism remains open until prototyping and evaluation are completed.

IPC interfaces should distinguish read, observe, action, and administrative operations according to the API architecture.

Processes should not depend on undocumented private communication channels for normal operation.

## Configuration and Process State

Process configuration should follow the ownership and scope rules defined by the configuration architecture.

System services should not use user configuration as an implicit source of privileged policy.

Runtime state should be distinguished from persistent configuration.

Where a configuration change affects a running service, the service should define whether the change is applied immediately, requires reload, or requires restart.

## Logging and Observability

Zethropol processes should integrate with established system logging and observability mechanisms.

Process-level observability should provide sufficient information to determine:

- Whether a service is running
- Whether a service is ready
- Whether a service has failed
- Why a service failed where diagnosable
- Whether a service is repeatedly restarting
- Whether resource usage is abnormal

Systemd and Linux logging mechanisms should be reused where appropriate.

## Security and Sandboxing

Process execution should use the minimum privileges and access required by the service.

Where appropriate, services should use operating-system mechanisms for restricting filesystem, device, network, and system-resource access.

Security hardening should be applied according to actual service requirements rather than through a one-size-fits-all process policy.

The final sandboxing and hardening profile for each service will be defined during service implementation and security validation.

## Desktop Independence

Core Zethropol system and service processes should not require KDE Plasma to operate unless the function is explicitly desktop-specific.

Hardware, monitoring, performance, power, diagnostics, update, recovery, and configuration services should remain usable independently from the desktop interface.

Desktop-specific processes may provide presentation and integration layers without becoming the authoritative source of system state.

## Service Process Candidates

The initial service model suggests the following execution candidates:

- Hardware Service: system-level service candidate
- Monitoring Service: system-level service candidate
- Performance Service: system-level service candidate
- Power Service: system-level service candidate
- Diagnostics Service: system-level service candidate
- Update Service: privileged system service candidate
- Recovery Service: privileged system service candidate
- Configuration Service: controlled system service candidate with user-session interaction

These are architectural candidates rather than final process or systemd unit definitions.

Multiple logical services may initially share an implementation process when isolation is not required.

## Process and Package Boundaries

Process boundaries and package boundaries should be considered separately.

A single package may contain one or more related processes, while a process may depend on components provided by multiple packages.

Packaging decisions should follow implementation and distribution requirements rather than forcing the logical service architecture into an identical package structure.

## Testing

Process architecture should be tested at both service and integration levels.

Testing should cover:

- Service startup
- Dependency unavailability
- Service restart
- Failure isolation
- Authorization boundaries
- IPC failures
- Configuration reload or restart behavior
- Resource-limit behavior where configured
- Logging and observability
- Clean shutdown

Services should remain independently testable even when the production implementation combines multiple logical services into one process.

## Relationship with Existing System Components

Zethropol process management should build upon Linux and systemd capabilities already provided by the CachyOS/Arch base system.

Zethropol should not replace systemd as the system service manager without a future architectural reason.

KDE Plasma remains the current desktop environment, but core Zethropol services should not require Plasma-specific process management.

## Current Architectural Status

The following process architecture principles are established:

- Separation of logical service and process concepts
- Privilege-aware process boundaries
- Separation of system and user-session execution
- Controlled service lifecycle
- Failure isolation
- Resource management
- Defined IPC boundaries
- Desktop independence
- Integration with systemd and Linux mechanisms
- Independent service testing

The following decisions remain open:

- Exact process layout
- Which logical services require independent processes
- Final systemd unit structure
- User-session service structure
- Exact service startup and readiness model
- Final resource-control policies
- Service-specific sandboxing and hardening
- Final IPC implementation

These decisions will be refined through service, IPC, and process prototyping.

## Process Architecture Status

This document represents the initial process architecture for Stage 2.

It is a working architectural document and will evolve as the Zethropol service execution model is validated.
