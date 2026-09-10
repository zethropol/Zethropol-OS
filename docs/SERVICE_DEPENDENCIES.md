# Zethropol OS — Service Dependency Architecture

## Purpose

This document defines the initial dependency model between Zethropol services and the underlying system architecture.

The dependency model is intended to keep service responsibilities clear, prevent circular dependencies, and preserve the separation between hardware access, system policy, services, and user-facing components.

## Dependency Principles

- Services should depend only on interfaces and components required for their responsibilities.
- Lower-level services should not depend on user-facing components.
- Services should avoid direct dependencies on unrelated services.
- Hardware-specific access should remain below normalized service interfaces.
- Circular service dependencies should be avoided.
- Existing Linux, systemd, and CachyOS infrastructure should be reused where appropriate.
- Dependencies should be explicit and independently testable.

## Dependency Layers

The initial service dependency direction is:

Hardware and Kernel Interfaces
        ↓
Base System Components
        ↓
Zethropol System Layer
        ↓
Core Zethropol Services
        ↓
User-Facing Components

Dependencies should normally flow downward through these layers.

User-facing components may consume service interfaces, but services must not depend on the user interface.

## Initial Service Dependencies

### Hardware Service

The Hardware Service provides normalized hardware information to higher-level services.

It may depend on:

- Linux kernel interfaces
- sysfs and procfs where appropriate
- Hardware monitoring interfaces
- Device discovery mechanisms

It should not depend on user-facing components.

### Monitoring Service

The Monitoring Service collects and exposes runtime system state.

It may consume data from:

- Hardware Service
- Kernel and system interfaces
- Base system monitoring mechanisms

The Monitoring Service should provide normalized runtime information to user-facing components.

### Performance Service

The Performance Service manages supported system performance policies and profiles.

It may depend on:

- Hardware Service
- Power Service
- Kernel power and CPU interfaces
- Existing system performance mechanisms

Performance policy decisions should remain separate from the user interface.

### Power Service

The Power Service manages and reports power-related state and policy.

It may depend on:

- Hardware Service
- Kernel power interfaces
- Battery and AC power interfaces
- Existing system power-management components

### Diagnostics Service

The Diagnostics Service performs controlled system and hardware diagnostics.

It may consume information from:

- Hardware Service
- Monitoring Service
- System and kernel interfaces

Diagnostic operations should not require the user interface to understand low-level implementation details.

### Update Service

The Update Service coordinates system update operations.

It may depend on:

- CachyOS and Arch package infrastructure
- pacman and ALPM
- Repository configuration
- Configuration Service where appropriate

The Update Service should not replace the underlying package-management infrastructure.

### Recovery Service

The Recovery Service coordinates supported recovery, rollback, and restoration operations.

It may depend on:

- System storage and filesystem interfaces
- Boot and system configuration mechanisms
- Update Service
- Configuration Service
- Existing recovery or rollback capabilities provided by the base system

Recovery operations must remain isolated from ordinary read-only service operations.

### Configuration Service

The Configuration Service provides controlled access to Zethropol system configuration.

It may depend on:

- Zethropol System Layer
- Persistent configuration storage
- Authorization mechanisms
- Existing system configuration interfaces

Configuration changes should be validated before being applied.

## Dependency Direction

The intended logical dependency relationships are:

Hardware Service
        ↓
Monitoring Service

Hardware Service
        ↓
Performance Service
        ↓
Power Service

Hardware Service
        ↓
Power Service

Hardware Service + Monitoring Service
        ↓
Diagnostics Service

Configuration Service
        ↓
Update Service

Configuration Service + Update Service
        ↓
Recovery Service

These relationships represent logical service dependencies, not mandatory startup order, process topology, or implementation structure.

Services that do not have a direct dependency relationship should remain independent even when they operate at the same architectural level.

## Service-to-Service Rules

Service-to-service dependencies should be kept minimal and explicit.

A service should consume another service through its defined interface rather than accessing that service internal implementation directly.

Where multiple services require the same low-level information, the shared responsibility should be provided by an appropriate lower-level service rather than duplicated independently.

Services should not access another service internal files, private state, or implementation-specific process details.

## Circular Dependency Prevention

Circular dependencies between services should not be permitted.

If two services appear to require each other directly, the shared responsibility should be moved to a lower-level interface or a separate supporting component.

For example, a monitoring service may consume normalized hardware information, while the hardware service must not depend on the monitoring service to obtain that information.

This rule prevents service initialization problems, hidden coupling, and difficult-to-test runtime behavior.

## User-Facing Dependency Boundary

User-facing components should consume Zethropol service APIs rather than directly accessing privileged system interfaces.

The user interface may depend on multiple services simultaneously, but those services remain independent of the user interface.

This boundary allows the service layer to remain reusable across different user interfaces and future desktop environments.

## Initialization and Runtime Dependencies

Service startup order should be determined by actual runtime dependencies rather than by arbitrary service numbering.

Services should expose clear readiness and failure states where required.

A service should not assume that another service is available merely because its process has started.

Where systemd is used for service lifecycle management, dependency and ordering relationships should be expressed through appropriate systemd mechanisms.

## Testability

Each service should be testable independently from user-facing components.

Service interfaces should allow controlled testing of normal operation, invalid input, unavailable hardware, authorization failures, and underlying system failures.

Dependencies should be replaceable or mockable where practical so that service behavior can be tested without requiring every physical hardware component.

## Relationship with the Base System

Zethropol services should build on existing Linux, systemd, CachyOS, and Arch infrastructure where appropriate.

Zethropol should add policy, normalization, integration, and user-facing capabilities rather than unnecessarily duplicating functionality already provided by the base system.

The dependency model must therefore distinguish between a Zethropol service dependency and an underlying system capability that the service consumes.

## Current Architectural Status

The following dependency principles are established:

- Clear dependency direction
- Explicit service-to-service dependencies
- No circular service dependencies
- Separation between services and user-facing components
- Hardware-specific access below normalized interfaces
- Explicit runtime dependencies
- Independent service testability
- Reuse of existing base-system infrastructure

The exact process layout, systemd unit relationships, package boundaries, and implementation-level dependencies remain open.

These decisions will be refined during service prototyping and integration.

## Dependency Architecture Status

This document represents the initial service dependency architecture for Stage 2.

It is a working architectural document and will evolve as service interfaces and prototypes are validated.
