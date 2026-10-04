# Zethropol OS — Service Architecture

## Purpose

This document defines the initial service boundaries of Zethropol OS.

Zethropol services provide reusable system-level functionality between the Zethropol System Layer and user-facing components.

The service architecture is intentionally modular. Services should have clear responsibilities and should communicate through defined interfaces rather than exposing low-level implementation details directly to user interfaces.

## Service Principles

- Each service should have a clearly defined responsibility.
- Services should avoid unnecessary duplication of existing system functionality.
- Hardware-specific implementation should remain below the service interface.
- User-facing applications should consume normalized service data.
- Privileged operations should be isolated behind controlled service boundaries.
- Services should be independently testable where practical.
- Service boundaries may evolve as implementation requirements become clearer.

## Initial Service Groups

### Hardware Service

Provides normalized information about detected hardware.

Expected responsibilities:

- CPU information
- GPU information
- Memory information
- Storage information
- Network hardware information
- Power and battery hardware information
- Thermal sensor information

The Hardware Service should hide hardware-specific paths and detection mechanisms from higher layers.

### Monitoring Service

Provides current and observable system-state information.

Expected responsibilities:

- CPU utilization
- GPU utilization
- Memory usage
- Storage activity
- Network activity
- Temperature information
- Relevant system health indicators

The Monitoring Service should provide normalized data suitable for consumption by the Zethropol System Monitor and other system components.

### Performance Service

Provides control over system performance behavior.

Potential responsibilities:

- Performance profiles
- CPU performance policies
- GPU performance policies where supported
- Power/performance balancing
- Application or workload-oriented performance modes

The Performance Service integrates with existing Linux/CachyOS performance and power-management mechanisms rather than replacing them.

### Power Service

Provides power-related system information and controls.

Expected responsibilities:

- Battery state
- Charging state
- Power source information
- Power-related system policies
- Power-saving behavior

Desktop and laptop-specific behavior may differ and should be handled through hardware capabilities rather than separate duplicated architectures.

### Diagnostics Service

Provides system diagnostics and health information.

Potential responsibilities:

- Hardware health checks
- System service checks
- Storage health information
- Thermal condition checks
- Configuration consistency checks
- Diagnostic data collection

Diagnostics should provide useful information without exposing unnecessary low-level implementation details to the user interface.

### Update Service

Provides controlled system update operations.

Potential responsibilities:

- Update availability
- Update information
- Update preparation
- Controlled update execution
- Update status reporting
- Update result reporting

The service should integrate with the existing Arch/CachyOS package infrastructure rather than replacing pacman/ALPM.

### Recovery Service

Provides system recovery and rollback capabilities.

Potential responsibilities:

- Recovery-state detection
- Snapshot or rollback integration
- Recovery preparation
- Rollback operations
- Recovery status reporting

The exact implementation will depend on the filesystem, snapshot technology, package state, and release architecture selected by Zethropol.

### Configuration Service

Provides centralized access to Zethropol-specific configuration.

Potential responsibilities:

- Reading Zethropol configuration
- Validating configuration
- Applying configuration changes
- Providing configuration state to other services
- Maintaining separation between system and user configuration

The final configuration storage format and location remain open architectural decisions.

## Service Interaction

The intended high-level relationship is:

```text
Zethropol User Space
        ↓
Zethropol Service Interface
        ↓
┌───────────────────────────────┐
│ Hardware      Monitoring      │
│ Performance   Power           │
│ Diagnostics   Update          │
│ Recovery      Configuration   │
└───────────────────────────────┘
        ↓
Zethropol System Layer
        ↓
Linux / systemd / CachyOS / Arch
```

The listed services represent logical service boundaries.

A logical service boundary does not automatically require a separate process, package, or systemd unit. The final process and packaging layout is determined by responsibility, privilege, lifecycle, failure isolation, resource usage, and communication requirements.

Production services that require system-wide execution may use dedicated systemd-managed processes where appropriate.

## Privilege Boundaries

Services that only provide system information should operate with the minimum privileges required to perform their functions.

Operations that modify system state should be isolated and explicitly authorized.

In particular:

- Read-only information should not require unrestricted administrative privileges.
- Configuration changes should pass through controlled interfaces.
- Performance and power changes should be validated before application.
- Update and recovery operations should use explicit authorization.
- User interfaces should not directly execute privileged system commands.

## Service Communication

Services are expected to communicate through the Zethropol service interface.

D-Bus is the validated primary IPC mechanism for Zethropol service communication.

Production system services use the established Zethropol D-Bus identity convention:

- Bus name: org.zethropol.<Service>
- Object path: /org/zethropol/<Service>
- Interface: org.zethropol.<Service>.v1

Where system-level service activation is required, D-Bus activation should integrate with systemd. A system service may therefore be activated on demand through D-Bus and managed by systemd for lifecycle, restart, dependency, and resource-control behavior.

The service interface should provide:

- Methods for controlled operations
- Signals or equivalent mechanisms for state changes
- Structured errors
- Clearly defined data types
- Authorization boundaries

## Hardware Independence

Services must not assume a specific hardware vendor, device model, sensor layout, or kernel interface.

For example, the Monitoring Service should expose a normalized GPU utilization value even when different GPUs provide that information through different kernel interfaces.

Hardware-specific detection belongs below the normalized service interface.

## Relationship with Existing System Components

Zethropol services should integrate with existing Linux infrastructure wherever practical.

Examples include:

- systemd for service lifecycle
- Linux kernel interfaces for hardware state
- procfs and sysfs where appropriate
- hwmon for thermal and sensor information
- DRM interfaces for graphics information
- ALPM/pacman for package management
- Existing power-management mechanisms where appropriate

Zethropol should add orchestration, normalization, policy, and user-facing functionality rather than unnecessarily replacing established components.

## Current Status

The following service boundaries are established as initial architectural concepts:

- Hardware Service
- Monitoring Service
- Performance Service
- Power Service
- Diagnostics Service
- Update Service
- Recovery Service
- Configuration Service

The following remain service-specific or implementation-level decisions:

- Process boundaries where isolation is required
- Service-specific systemd unit settings
- Service-specific D-Bus methods and data structures
- Service-specific authorization and polkit policies
- Configuration storage
- Service dependency relationships
- Service-specific sandboxing and resource controls

The common D-Bus identity, IPC, and system-service activation architecture is established and should be applied consistently across production services.
