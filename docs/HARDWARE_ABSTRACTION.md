# Zethropol OS — Hardware Abstraction Architecture

## Purpose

This document defines the initial hardware abstraction architecture for Zethropol OS.

The hardware abstraction architecture is intended to provide a consistent representation of hardware capabilities to higher-level Zethropol services without requiring those services to depend directly on hardware-specific implementation details.

Hardware-specific discovery and access should remain below stable Zethropol service interfaces.

## Hardware Abstraction Principles

- Hardware-specific implementation details should remain isolated below service boundaries.
- Higher-level services should consume normalized hardware information.
- Hardware detection should be capability-driven rather than based on assumptions about specific devices.
- Hardware interfaces should tolerate missing, unsupported, or changing hardware capabilities.
- Existing Linux kernel and system interfaces should be reused where appropriate.
- Hardware abstraction should remain independent from the desktop user interface.
- Hardware information should expose enough detail for Zethropol services without unnecessarily leaking implementation-specific paths.
- Hardware detection and normalization should be independently testable.

## Hardware Layer

The hardware layer consists of physical devices, firmware-exposed information, kernel drivers, and kernel interfaces used to discover and operate hardware.

Zethropol should not replace the Linux kernel hardware and driver model.

Instead, Zethropol should consume established interfaces such as device information, sysfs, procfs, hwmon, DRM, network interfaces, and other appropriate kernel or subsystem interfaces.

Hardware-specific implementation details should be translated into stable internal representations before being consumed by higher-level services.

## Hardware Discovery

Hardware discovery should identify available devices and capabilities during system operation.

Discovery should account for:

- CPU topology and capabilities
- GPU devices and capabilities
- Memory capacity and state
- Storage devices and capabilities
- Network interfaces and capabilities
- Power and battery capabilities
- Thermal sensors and cooling capabilities
- Other supported hardware exposed by the base system

Hardware discovery should not assume that every system provides the same devices, sensors, interfaces, or capabilities.

Hardware discovery results should be represented using stable Zethropol concepts.

## Normalized Hardware Model

Zethropol services should consume hardware information through normalized models rather than directly interpreting hardware-specific interfaces.

The normalized hardware model should represent common concepts such as:

- Device identity
- Device type
- Vendor and model information
- Availability state
- Supported capabilities
- Current operating state
- Performance-related information
- Power-related information
- Thermal information where available

Not every hardware device will provide every property.

Unavailable information should be represented explicitly rather than replaced with fabricated or misleading values.

## CPU Abstraction

The CPU abstraction should represent processor topology, logical and physical processing resources, supported capabilities, current operating state, and relevant frequency or utilization information where available.

CPU-specific kernel interfaces should remain below the abstraction boundary.

Higher-level services should not need to know which procfs, sysfs, or kernel interface provides a specific CPU property.

## GPU Abstraction

The GPU abstraction should represent available graphics devices, vendor and model information, driver association, utilization, memory information, frequency or performance state, and thermal information where available.

The abstraction should support systems with multiple GPUs and should not assume a single graphics device.

GPU-specific interfaces such as DRM or driver-specific sysfs and hwmon paths should remain implementation details below the service boundary.

## Memory Abstraction

The memory abstraction should represent system memory capacity, current usage, available memory, and other relevant memory state exposed by the base system.

Memory information should be normalized so higher-level services do not depend directly on procfs implementation details.

## Storage Abstraction

The storage abstraction should represent available storage devices, device identity, capacity, type, health information where available, and relevant operating state.

Storage devices may expose different capabilities depending on their type, controller, driver, and firmware.

The abstraction should therefore represent unsupported or unavailable properties explicitly.

## Network Abstraction

The network abstraction should represent available network interfaces, interface type, operational state, connectivity state, addressing information where appropriate, and traffic statistics.

Network interface names should not be treated as stable hardware identities.

The abstraction should support systems with multiple network interfaces and changing interface names.

## Power and Battery Abstraction

The power abstraction should represent available power sources, battery state where applicable, charging state, power-related capabilities, and relevant system power information.

The abstraction must support systems without batteries, such as desktop computers.

Power information should distinguish unavailable capabilities from inactive capabilities.

## Thermal Abstraction

The thermal abstraction should represent available thermal sensors, temperature readings, thermal zones, and cooling-related state where supported.

Thermal sensor availability and naming may vary between hardware platforms.

Hardware-specific sensor paths should therefore remain below the abstraction boundary.

Invalid, unavailable, or temporarily inaccessible sensor readings should be represented explicitly.

## Hardware Capabilities

Hardware capabilities should be represented independently from specific device models whenever practical.

Examples may include:

- CPU frequency control
- GPU performance states
- Hardware video acceleration
- Battery and charging support
- Temperature monitoring
- Storage health information
- Network interface capabilities

A capability should describe what the system can reliably perform rather than assuming that a particular hardware model provides that capability.

## Device Identity

Hardware devices should have stable identifiers where the underlying system provides them.

Human-readable vendor and model names may be exposed for presentation purposes but should not be used as the sole basis for identifying hardware.

Device paths, interface names, and dynamically assigned identifiers should not be treated as permanent identities unless their stability is guaranteed by the underlying subsystem.

## Dynamic Hardware

Hardware availability may change during system operation.

The hardware abstraction should account for devices or capabilities appearing, disappearing, becoming unavailable, or changing state.

Where the underlying subsystem supports reliable event notification, hardware changes should be observable by appropriate Zethropol services.

Consumers should not assume that previously discovered hardware remains available indefinitely.

## Hardware Service Boundary

The Hardware Service should provide the primary Zethropol service boundary for normalized hardware information.

The Hardware Service should be responsible for hardware discovery, normalization, capability reporting, and hardware state access required by other Zethropol services.

Higher-level services should consume the Hardware Service interface rather than accessing hardware-specific kernel interfaces directly.

The Hardware Service may use multiple underlying Linux interfaces and subsystem-specific mechanisms to construct the normalized representation.

The implementation should keep those mechanisms internal to the hardware service boundary.

## Unavailable and Error States

Hardware information may be unavailable because a device does not support a capability, a driver does not expose the required information, a sensor is inaccessible, or the hardware is temporarily unavailable.

These conditions should be represented explicitly through the service data and error model.

An unavailable property must not be interpreted as a zero value, an assumed default, or fabricated hardware information.

Hardware discovery failures should distinguish between a missing capability, a temporary access failure, and a more significant hardware or subsystem error where practical.

## Relationship with Zethropol Services

The Hardware Service provides normalized hardware information to services that require hardware awareness.

Monitoring Service may consume CPU, GPU, memory, network, storage, power, and thermal state.

Performance Service may consume hardware capabilities and performance-related state.

Power Service may consume power, battery, thermal, and hardware capability information.

Diagnostics Service may consume hardware identity, capability, state, and error information.

User-facing components should consume these services through their defined service interfaces rather than accessing hardware interfaces directly.

## Testability

Hardware abstraction should be independently testable without requiring every supported hardware configuration.

Tests should cover:

- Hardware discovery
- Multiple hardware devices
- Missing hardware capabilities
- Unsupported hardware interfaces
- Invalid or unavailable sensor data
- Hardware state changes
- Capability detection
- Stable device identification
- Service behavior when hardware becomes unavailable

Where practical, hardware-specific sources should be replaceable with test or simulated data so higher-level services can be tested independently.

## Relationship with the Base System

Zethropol should build upon the hardware discovery and driver mechanisms provided by Linux, systemd, the CachyOS/Arch base system, and relevant kernel subsystems.

Zethropol should not duplicate existing hardware management functionality without a clear architectural reason.

Where Zethropol provides a higher-level abstraction, the underlying hardware interface should remain an implementation detail below the appropriate service boundary.

## Current Architectural Status

The following hardware abstraction principles are established:

- Hardware-specific implementation isolation
- Normalized hardware models
- Capability-driven hardware detection
- Explicit unavailable and error states
- Support for multiple and dynamic hardware devices
- Hardware-independent service interfaces
- Separation from the user interface
- Reuse of existing Linux and base-system interfaces
- Independent hardware abstraction testing

The following decisions remain open:

- Exact normalized data schemas
- Stable hardware identifier strategy
- Exact Hardware Service API
- Hardware event and change notification model
- Exact discovery implementation
- Capability representation and versioning
- Hardware-specific override mechanisms

These decisions will be refined through Hardware Service and service-interface prototyping.

## Hardware Abstraction Architecture Status

This document represents the initial hardware abstraction architecture for Stage 2.

It is a working architectural document and will evolve as the Zethropol hardware and service models are validated.
