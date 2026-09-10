# Zethropol OS — Configuration Architecture

## Purpose

This document defines the initial architecture for configuration management in Zethropol OS.

The configuration architecture is intended to provide predictable, validated, and maintainable configuration while separating system-wide policy from user-specific preferences.

Configuration should remain independent from the user interface so that services and future desktop environments can consume the same configuration model.

## Configuration Principles

- Configuration should have clear ownership.
- System-wide configuration and user-specific configuration should remain logically separate.
- Configuration should use stable and documented formats.
- Services should validate configuration before applying changes.
- User interfaces should access configuration through defined service interfaces where appropriate.
- Privileged configuration changes should pass through explicit authorization boundaries.
- Configuration should not duplicate functionality already provided by the base system.
- Invalid or unsupported configuration should fail safely.

## Configuration Ownership

Each configuration value should have a clearly defined owner.

Zethropol configuration should generally fall into three categories:

- System configuration
- User configuration
- Hardware or capability-specific configuration

System configuration defines policies or behavior affecting the operating system as a whole.

User configuration defines preferences belonging to an individual user or session.

Hardware or capability-specific configuration defines settings that depend on detected hardware or supported capabilities.

Configuration ownership should prevent multiple services from independently modifying the same setting without coordination.

## Configuration Layers

Zethropol configuration should be organized according to ownership and scope rather than stored in a single global configuration location.

The initial logical model is:

System Configuration
        ↓
Zethropol Service Configuration
        ↓
Hardware and Capability Configuration
        ↓
User Configuration

This represents logical ownership and scope, not a mandatory precedence order or filesystem layout.

## System Configuration

System configuration contains settings that affect the operating system or multiple users.

Examples may include:

- System-wide policies
- Default performance policies
- Power-management policies
- Update policies
- Recovery policies
- Zethropol service behavior

System configuration may require administrative authorization to modify.

## User Configuration

User configuration contains preferences that belong to an individual user.

Examples may include:

- User-selected performance preferences
- User interface preferences
- Notification preferences
- User-specific service preferences

User configuration should not require administrative privileges unless it changes a system-wide policy.

## Hardware and Capability Configuration

Hardware-specific configuration should be derived from detected capabilities where possible.

Hardware-dependent settings should not assume that a specific device, driver, or sysfs path is always present.

Where hardware-specific overrides are required, they should be associated with stable hardware or capability identifiers rather than fragile implementation-specific paths.

## Configuration Format

Configuration formats should be human-readable where practical and suitable for reliable machine parsing.

The selected format should support:

- Explicit data types
- Stable key names
- Clear validation rules
- Safe handling of unknown or unsupported values
- Future extension without unnecessary breaking changes

The final configuration format will be selected after evaluating the requirements of the service layer and the existing base-system configuration mechanisms.

## Defaults and Precedence

Configuration values should have predictable defaults.

When multiple configuration sources provide a value, precedence must be explicitly defined.

Hardware capabilities, system policy, service configuration, and user preferences should not override one another implicitly.

Unsupported combinations should be detected and rejected or resolved through a defined policy rather than producing undefined behavior.

## Configuration Changes

Configuration changes should be validated before they are applied.

Where practical, services should distinguish between:

- Requested configuration
- Validated configuration
- Active configuration

Changes that require privileged access should pass through the appropriate authorization mechanism.

Services should report configuration errors using the structured API error model defined by the service API architecture.

## Atomicity and Safety

Configuration changes should avoid leaving the system in a partially modified state.

Where a configuration update modifies multiple related values, the operation should be applied as a single logical change where practical.

Failed configuration changes should preserve the last known valid configuration whenever possible.

Configuration mechanisms should avoid silently accepting malformed or incompatible values.

## Configuration Service

The Configuration Service is the controlled entry point for Zethropol-managed configuration.

It should provide:

- Reading configuration values
- Validating proposed changes
- Applying authorized changes
- Reporting configuration state
- Reporting configuration errors

Other services should not directly modify configuration owned by the Configuration Service.

Services may consume configuration through defined interfaces or controlled configuration access mechanisms.

## Service Configuration

Individual services may have service-specific configuration.

Service configuration should remain scoped to that service unless a setting represents a shared system policy.

Shared settings should have a clearly defined owner to prevent conflicting configuration management.

## Security and Authorization

Configuration access must follow the principle of least privilege.

Read access should require only the permissions necessary to expose the requested configuration.

Changes to system-wide or privileged configuration must require explicit authorization.

User-facing components must not receive unrestricted write access to privileged configuration.

Configuration values originating from user interfaces or external clients must be treated as untrusted input and validated before use.

## Recovery and Configuration

Configuration changes should be compatible with the Zethropol recovery architecture.

Where practical, important system configuration should be recoverable to a previously known valid state.

Configuration backup and restoration mechanisms should be defined together with the broader recovery architecture rather than implemented independently by individual services.

## Relationship with the Base System

Zethropol configuration should integrate with existing Linux, systemd, CachyOS, and Arch configuration mechanisms where appropriate.

Zethropol should not duplicate an existing base-system configuration mechanism without a clear architectural reason.

Where Zethropol provides a higher-level abstraction, the underlying system configuration should remain an implementation detail below the appropriate service boundary.

## Testability

Configuration handling should be independently testable.

Tests should cover:

- Valid configuration
- Invalid configuration
- Missing configuration
- Unsupported values
- Conflicting values
- Permission and authorization failures
- Configuration changes that fail during application

## Current Architectural Status

The following configuration principles are established:

- Clear configuration ownership
- Separation of system and user configuration
- Hardware and capability-aware configuration
- Validated configuration changes
- Explicit configuration precedence
- Least-privilege access
- Safe handling of configuration failures
- Integration with the recovery architecture
- Reuse of existing base-system mechanisms

The following decisions remain open:

- Final configuration file format
- Exact filesystem locations
- Configuration schema
- Configuration precedence rules
- Runtime reload behavior
- Backup and restoration implementation
- Exact Configuration Service API

These decisions will be refined through service prototyping and system integration.

## Configuration Architecture Status

This document represents the initial configuration architecture for Stage 2.

It is a working architectural document and will evolve as the service and configuration models are validated.
