# Zethropol OS — Hardware Service

## Purpose

The Hardware Service provides the primary Zethropol service boundary for hardware discovery, normalized hardware information, capability detection, and hardware state.

The service isolates hardware-specific implementation details from higher-level Zethropol services and user-facing components.

## Responsibilities

The Hardware Service is responsible for:

- Discovering available hardware
- Identifying relevant hardware devices
- Collecting supported hardware properties
- Normalizing hardware information
- Reporting hardware capabilities
- Representing unavailable and unsupported properties explicitly
- Detecting relevant hardware availability changes
- Providing hardware information to other Zethropol services

The service should not own functionality that belongs to higher-level services such as performance policy, power policy, diagnostics, updates, or user-interface presentation.

## Hardware Domains

The initial hardware model covers:

- CPU
- GPU
- Memory
- Storage
- Network
- Power and battery
- Thermal sensors and thermal state

Additional hardware domains may be added as the architecture develops.

## Hardware Discovery

Hardware discovery should use established Linux interfaces and system facilities where practical.

Potential sources include:

- sysfs
- procfs
- hwmon
- DRM and graphics interfaces
- PCI and USB device information
- Kernel-provided device information
- Existing system hardware discovery mechanisms

Hardware-specific access should remain internal to the Hardware Service implementation.

## Normalized Hardware Model

The service should expose normalized Zethropol hardware concepts rather than raw kernel or driver data.

For example, consumers should receive a normalized GPU model containing relevant utilization, memory, frequency, temperature, and capability information instead of being required to know a specific sysfs or hwmon path.

Normalized models should remain stable across supported hardware implementations where the underlying capability is equivalent.

## Capability Detection

Hardware capabilities must be detected explicitly.

A capability may be:

- Supported and available
- Supported but temporarily unavailable
- Unsupported
- Restricted
- Failed or inaccessible

Consumers must not assume that a hardware feature exists solely because the corresponding service interface exists.

## Unavailable Data

The Hardware Service must distinguish unavailable data from valid zero or default values.

Examples include:

- A temperature sensor that is unavailable
- GPU memory information that is not exposed by the driver
- Battery information on a desktop system
- A frequency value that cannot currently be read

Unavailable information should be represented explicitly through the service data model.

## Dynamic Hardware

The service should account for hardware that may appear, disappear, or change state during system operation.

Examples include:

- USB devices
- External displays
- Network interfaces
- Hot-pluggable storage
- GPU availability changes

Where reliable event mechanisms exist, relevant changes should be exposed through the service interface rather than requiring continuous client polling.

## Service Interface

The Hardware Service should expose interfaces for:

- Hardware inventory
- Hardware properties
- Hardware capabilities
- Hardware availability
- Relevant hardware state changes

The exact method, property, and signal definitions will be established during service-interface prototyping.

## Service Dependencies

The Hardware Service should minimize dependencies on higher-level Zethropol services.

It should primarily depend on:

- Linux kernel interfaces
- Hardware discovery mechanisms
- Base system facilities where required
- Zethropol System Layer interfaces where defined

The Hardware Service must not depend on user-facing components.

## Relationship with Other Services

Other Zethropol services should consume normalized hardware information through the Hardware Service.

Examples include:

- Monitoring Service consumes hardware state and utilization information.
- Performance Service consumes hardware capabilities and performance-related state.
- Power Service consumes power and hardware capability information.
- Diagnostics Service consumes hardware inventory and hardware state.

This establishes the Hardware Service as a foundational service for higher-level system functionality.

## Privilege Boundary

The Hardware Service should operate with the minimum privileges required to discover and read hardware information.

Hardware access that does not require elevated privileges should remain unprivileged.

Privileged hardware operations, if required in the future, should be explicitly separated and authorized rather than granting unrestricted hardware access to the service.

## Error Handling

Hardware discovery and access failures must be represented explicitly.

The service should distinguish between:

- Device not found
- Capability unsupported
- Data temporarily unavailable
- Access denied
- Driver or kernel interface failure
- Invalid hardware state

One unavailable hardware property must not cause unrelated hardware information to become unavailable.

## Testing

The Hardware Service should be independently testable.

Testing should cover:

- Hardware discovery
- Hardware identification
- Normalized data generation
- Capability detection
- Missing hardware
- Unsupported capabilities
- Temporary access failures
- Dynamic hardware changes
- Invalid or incomplete kernel data

Testing should support mocked or simulated hardware data where practical so that service behavior is not dependent on one physical machine.

## Relationship with Existing System Components

The Hardware Service should build upon Linux kernel interfaces and the existing CachyOS/Arch base system.

It should not replace established kernel or system hardware-management mechanisms without a clear architectural reason.

## Implementation Status

The Hardware Service has a production D-Bus implementation.

The service is implemented in Rust and exposes normalized hardware state through the system D-Bus.

### D-Bus Identity

- Bus name: org.zethropol.Hardware
- Object path: /org/zethropol/Hardware
- Interface: org.zethropol.Hardware.v1

### Current Interface

The production interface currently exposes:

- GetState

GetState returns the normalized hardware state using the standard Zethropol structured result model:

- success
- code
- message
- operationId
- data

The data payload contains normalized hardware information collected dynamically from the running system.

### Implementation

The service currently integrates hardware modules for:

- CPU
- GPU
- Memory
- Storage
- Network
- Power
- System information
- Hardware diagnostics
- Hardware intelligence
- Hardware fingerprinting
- Hardware change detection

The service remains independent of user-facing components.

### Deployment

The production service is designed for system-level D-Bus activation through systemd.

Deployment definitions are maintained under:

- deployment/dbus/
- deployment/systemd/

The packaged runtime binary is installed under:

- /usr/lib/zethropol/zethropol-hardware

The D-Bus service is activated on demand and managed by systemd.

### Service Relationship

The Hardware Service is a foundational Zethropol service.

Higher-level services consume its normalized state rather than accessing hardware-specific Linux interfaces directly.

Examples:

- Monitoring consumes hardware telemetry.
- Performance consumes hardware capabilities and performance-related state.
- Power consumes power and hardware capability information.
- Diagnostics consumes hardware inventory and hardware state.

### Development Compatibility

The command-line modes currently retained by the Hardware binary provide compatibility for existing development and diagnostic workflows.

The production service path uses the D-Bus interface and does not depend on user-specific paths under /home.

## Hardware Service Status

The Hardware Service is implemented as a production Stage 3 — Hardware Intelligence service.

The foundational D-Bus boundary and systemd activation model are established.

Future work may extend the interface with additional methods or signals where required by the Zethropol service architecture.
