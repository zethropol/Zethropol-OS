# Zethropol OS — System Architecture

## Purpose

This document defines the initial system architecture of Zethropol OS.

The architecture is designed to build on the existing CachyOS and Arch Linux foundation rather than replacing established components without a clear technical reason.

This document represents the initial Stage 2 architecture and may be revised as implementation and testing reveal new requirements.

## Architectural Principles

- Reuse proven open-source infrastructure where appropriate.
- Keep Zethropol-specific functionality separated from the underlying Linux base.
- Separate system logic from user interfaces.
- Keep hardware-specific implementation details away from user-facing components.
- Use defined interfaces between architectural layers.
- Apply privilege boundaries to operations that can modify the system.
- Avoid unnecessary duplication of functionality already provided by the base system.

## Layer Model

```text
Hardware
    ↓
Linux Kernel
    ↓
CachyOS / Arch Base
    ↓
Zethropol System Layer
    ↓
Zethropol Service Layer
    ↓
Zethropol User Space
    ↓
KDE Plasma / Desktop
```

The diagram represents logical responsibilities rather than a requirement that every layer be implemented as a separate package or process.

## Linux Kernel

The Linux kernel provides the fundamental interface between Zethropol OS and the hardware.

Responsibilities include:

- CPU scheduling and execution
- Memory management
- Hardware drivers
- Storage and networking primitives
- Kernel-level power management facilities
- Kernel interfaces exposed to user space

Zethropol should not replace the kernel architecture with a separate Zethropol kernel without a future technical requirement that justifies such a decision.

## CachyOS / Arch Base

The base layer provides the established Linux distribution infrastructure on which Zethropol is built.

This includes, where appropriate:

- Package management and ALPM/pacman infrastructure
- systemd
- Core system libraries and utilities
- Arch Linux package and repository infrastructure
- CachyOS-specific optimizations and components

Zethropol should integrate with these components instead of unnecessarily replacing them.

## Zethropol System Layer

The System Layer is the primary Zethropol-specific system abstraction layer.

Its responsibilities are expected to include:

- Zethropol system configuration
- Hardware abstraction and normalization
- System policies
- Integration between the base operating system and Zethropol services
- Common interfaces used by higher layers

The System Layer should not depend directly on a specific graphical desktop environment.

## Zethropol Service Layer

The Service Layer provides reusable system services built on top of the System Layer.

Potential services include:

- Hardware information
- System monitoring
- Diagnostics
- Performance management
- Power management
- System health
- Update management
- Recovery and rollback

The final service boundaries, process model, and service names are not yet defined.

Services may integrate with systemd for lifecycle management where appropriate.

## Zethropol User Space

The User Space contains user-facing Zethropol applications and interfaces.

Potential components include:

- Zethropol Control Center
- System Monitor
- Diagnostics interface
- Performance profile interface
- Maintenance tools
- Software management interface

User-facing applications should obtain system information and request privileged operations through defined Zethropol interfaces rather than directly implementing low-level hardware access throughout each application.

## KDE Plasma

KDE Plasma is the current default desktop environment of Zethropol OS.

Zethropol components may integrate with Plasma, but the core Zethropol system services should not depend on Plasma-specific APIs unless there is a clear architectural reason.

This separation preserves the possibility of supporting alternative desktop environments in the future.

## Layer Communication

The target communication model is:

```text
User Interface
      ↓
Zethropol Service Interface
      ↓
Zethropol Service Layer
      ↓
Zethropol System Interface
      ↓
Zethropol System Layer
      ↓
Linux / systemd / hardware interfaces
```

The interfaces should expose stable Zethropol concepts rather than raw implementation details.

For example, a user interface should request a normalized GPU load value rather than depending directly on a particular sysfs or hwmon path.

## API Requirements

The future Zethropol service interface should support four broad categories:

### Read

Read system and hardware state without modifying the system.

### Observe

Receive notifications when relevant system state changes.

### Action

Request controlled non-administrative system operations.

### Admin

Request operations that require elevated authorization or can materially modify system state.

The distinction between these categories is intended to provide a clear foundation for authorization and security decisions.

## IPC Decision

An IPC mechanism is required for communication between Zethropol components.

D-Bus is currently the primary candidate because its interface, object, method, signal, and error model fits the identified communication requirements and integrates naturally with the Linux/systemd ecosystem.

However, D-Bus is not yet considered the final architectural decision.

The final IPC mechanism should be selected after a small prototype evaluates:

- API complexity
- Performance characteristics
- Error handling
- Service activation
- Authorization and policy integration
- Debugging and observability
- Compatibility with future Zethropol components

## Privilege Model

Zethropol should distinguish between operations that only read system state and operations that modify system state.

User interfaces should not receive unrestricted system privileges simply because they are part of Zethropol.

Privileged operations should pass through controlled service boundaries and an explicit authorization mechanism.

The detailed authorization implementation belongs to a later security architecture stage.

## Hardware Abstraction

Hardware-specific implementation details should be isolated below the user-facing layers.

A hardware interface should provide normalized concepts such as:

- CPU information and utilization
- GPU information and utilization
- Memory state
- Storage state
- Network state
- Power and battery state
- Thermal state

This allows higher-level components to remain independent of individual hardware paths and sensor layouts.

## Current Prototype Relationship

The existing Zethropol System Monitor prototype is considered a development and research prototype.

Its direct access to Linux interfaces such as procfs, sysfs, hwmon, and DRM-related information is useful for discovering available data and validating hardware access.

The prototype should not be treated as the final architectural implementation.

Future System Monitor development should consume the appropriate Zethropol interfaces once the relevant system services have been defined.

## Current Architectural Status

The following decisions are currently established:

- Zethropol is built on CachyOS and Arch Linux.
- The Linux kernel and established base infrastructure remain foundational components.
- Zethropol introduces its own System Layer.
- Zethropol introduces a Service Layer above the System Layer.
- User-facing Zethropol components should remain separated from low-level hardware access.
- KDE Plasma remains the current default desktop environment.
- A defined IPC interface is required between Zethropol components.

The following decisions remain open:

- Final IPC mechanism
- Exact service boundaries
- Exact API naming and object model
- Authorization implementation
- Configuration storage model
- Packaging and process layout

## Architecture Status

This document represents the initial architecture for Stage 2. It is a working architectural document and is expected to evolve through research, prototyping, implementation, and testing.
