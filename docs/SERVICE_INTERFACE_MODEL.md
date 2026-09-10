# Zethropol OS — Service Interface Model

## Purpose

This document defines the logical interface model used by Zethropol services.

The service interface model establishes how services expose capabilities and operations to other Zethropol components without exposing implementation-specific details.

The model is independent of the final IPC technology and may later be mapped to D-Bus or another IPC mechanism.

## Interface Principles

- Each service should expose a clearly defined interface.
- Interfaces should represent stable Zethropol concepts.
- Interfaces should expose only the functionality required by consumers.
- Implementation details should remain behind the service boundary.
- Interfaces should be hardware-independent where practical.
- Privileged operations must have explicit authorization requirements.
- Inputs, outputs, state, and errors should use structured data.
- Interfaces should support capability detection and unavailable states.
- Interfaces should be versionable and capable of controlled evolution.
- Interfaces should remain usable independently of the graphical desktop.

## Service Identity

Each service should have a stable logical identity within the Zethropol architecture.

Service identity should describe the responsibility of the service rather than the process, executable, or implementation technology used to provide it.

The logical identity should remain stable even if the internal implementation changes.

Initial logical service identities include:

- Hardware Service
- Monitoring Service
- Performance Service
- Power Service
- Diagnostics Service
- Update Service
- Recovery Service
- Configuration Service

## Interface Structure

A service interface may contain:

- Methods for explicit operations
- Properties or state representations where appropriate
- Signals or events for state changes
- Capability information
- Interface version information

The exact representation will depend on the final IPC implementation.

## Operation Categories

The interface model uses four operation categories defined by the Zethropol API architecture.

### READ

READ operations retrieve current state without modifying system state.

Examples include:

- Reading CPU utilization
- Reading GPU utilization
- Reading available memory
- Reading power state
- Reading service status

READ operations should not require administrative authorization unless the underlying information itself is restricted.

### OBSERVE

OBSERVE represents subscription to relevant state changes or events.

Examples include:

- Hardware availability changes
- Thermal state changes
- Power changes
- Performance profile changes
- Configuration changes
- Service state changes

Observation should avoid unnecessary polling when reliable event notification is available.

### ACTION

ACTION operations perform controlled operations that do not inherently require administrative privileges.

Examples may include:

- Requesting a diagnostic refresh
- Starting a non-privileged monitoring operation
- Requesting a service state refresh

Each ACTION operation must still validate its input and enforce applicable policy.

### ADMIN

ADMIN operations modify privileged or system-wide state.

Examples may include:

- Changing system performance policy
- Applying system configuration
- Starting a recovery operation
- Applying system updates

ADMIN operations must pass explicit authorization before the operation is performed.

## Methods

Methods represent explicit service operations.

Each method should define:

- Operation name
- Operation category
- Required inputs
- Expected outputs
- Possible errors
- Authorization requirements
- Relevant state transitions

Methods should be deterministic where practical and should not rely on undocumented side effects.

Long-running operations should use an asynchronous model where synchronous waiting would create unreliable or excessive client blocking.

## Properties and State

Services may expose properties representing stable service state where this improves interoperability and observability.

Properties should represent meaningful Zethropol concepts rather than raw implementation values.

Examples include:

- Service availability
- Service readiness
- Current performance profile
- Current power state
- Hardware capability state

Properties should not expose internal mutable state unless that state is intentionally part of the public interface.

## Signals and Events

Services may expose signals or events for state changes that consumers need to observe.

Events should contain sufficient structured information to identify the change without exposing unnecessary implementation details.

An event should not require consumers to understand the internal mechanism that caused the change.

## Capability Model

Interfaces should support explicit capability information where functionality depends on hardware or system support.

Capabilities may represent conditions such as:

- Supported
- Unsupported
- Available
- Temporarily unavailable
- Restricted
- Failed

Consumers should query capability state rather than assuming that every operation exists on every system.

## Data Model

Interface data should use structured Zethropol models.

Hardware-specific paths, device nodes, kernel paths, driver-specific names, and internal implementation details should remain below the service boundary.

For example, a consumer should receive a normalized GPU utilization value rather than a driver-specific sysfs path.

Unavailable or unsupported values must remain distinguishable from valid zero values.

## Input Validation

Every method accepting external input must validate that input before processing it.

Validation should consider:

- Type and structure
- Allowed values
- Numeric ranges
- Required fields
- Capability availability
- Current system state
- Authorization requirements

Invalid input must result in a structured error and must not produce an unintended state change.

## Output Model

Method outputs should use structured data with clearly defined semantics.

Outputs should distinguish between:

- Valid values
- Unavailable values
- Unsupported capabilities
- Temporary failure
- Permanent failure where relevant

Human-readable text may be provided for diagnostics but should not be the primary machine-readable interface.

## Error Model

Interface errors should follow the structured error model defined by the Zethropol API architecture.

Initial error categories include:

- Invalid request
- Unsupported operation
- Missing capability
- Authorization failure
- Service unavailable
- Dependency unavailable
- Resource unavailable
- Operation failure
- Timeout or communication failure

Errors should provide sufficient diagnostic information while avoiding unnecessary disclosure of sensitive implementation details.

## Authorization Boundary

Authorization belongs at the service boundary for privileged operations.

A consumer must not bypass a service authorization boundary by directly accessing the underlying resource.

The receiving service should evaluate:

- Requesting identity
- Requested operation
- Requested parameters
- Current system state
- Applicable security policy

The final authorization mechanism and policy remain open for implementation-stage definition.

## Service Dependencies

A service interface may consume another service interface when a dependency is explicitly defined by the service architecture.

Consumers should depend on published interfaces rather than private implementation state.

For example:

- Monitoring Service consumes Hardware Service interfaces.
- Performance Service consumes Hardware and Power Service interfaces.
- Diagnostics Service consumes Hardware and Monitoring Service interfaces.
- Update Service consumes package-management and Configuration Service interfaces where required.

Circular service dependencies should not be introduced.

## Hardware Independence

Hardware-dependent implementation must remain below the Hardware Service boundary.

Other services should consume normalized hardware information through the Hardware Service interface.

This allows the same service interface to operate across different CPU, GPU, storage, network, thermal, and power implementations.

## Service Availability and Readiness

Interfaces should distinguish service availability from readiness where practical.

A service may be registered but still initializing or waiting for a dependency.

Consumers should be able to detect:

- Service unavailable
- Service initializing
- Service ready
- Service degraded
- Service unavailable because of dependency failure

Consumers must handle service disappearance and reappearance without assuming a permanent connection.

## Long-Running Operations

Operations that may take significant time should expose an asynchronous execution model where appropriate.

An asynchronous operation should provide a way for the consumer to determine:

- Whether the operation was accepted
- Current operation state where applicable
- Completion
- Failure
- Cancellation support where available

The exact asynchronous model remains open for IPC prototyping.

## Interface Versioning

Interfaces must support controlled evolution.

Versioning should prevent a client from silently interpreting changed semantics as the original interface.

Backward-compatible additions should be preferred where practical.

Breaking changes should use an explicit version transition or another clearly defined compatibility mechanism.

Clients should be able to determine the interface version and relevant capabilities provided by a service.

## Naming

Service and interface names should describe stable Zethropol concepts.

Names should not be based on:

- Specific executable names
- Process IDs
- Hardware vendor names
- Driver-specific paths
- Temporary implementation details

The final naming convention for service identifiers, interfaces, methods, properties, and signals will be established during IPC prototyping.

## IPC Mapping

The logical service interface model is independent of the final IPC technology.

D-Bus is the current primary IPC candidate and may provide the concrete transport and object/interface representation.

The mapping should preserve the logical interface model rather than allowing IPC-specific implementation details to redefine service responsibilities.

## Testing

Service interfaces should be independently testable.

Testing should cover:

- Valid method requests
- Invalid input
- Capability detection
- Structured outputs
- Structured errors
- Authorization failures
- Service unavailability
- Dependency failures
- State changes and events
- Interface version compatibility
- Multiple simultaneous consumers

Interface tests should not require a complete graphical desktop environment.

## Relationship with User Space

User-facing components should consume service interfaces rather than directly accessing system resources owned by Zethropol services.

For example, a Control Center or System Monitor should request normalized service data rather than directly reading hardware-specific kernel interfaces.

This separation allows the user interface to remain focused on presentation and interaction while services retain responsibility for system state and policy.

## Relationship with KDE Plasma

KDE Plasma may consume Zethropol service interfaces as a user-facing environment.

Core service interfaces should not require Plasma-specific APIs unless a feature is explicitly desktop-specific.

This preserves the possibility of supporting additional desktop environments in the future.

## Current Architectural Status

The following interface principles are established:

- Stable logical service identities
- Defined operation categories
- Structured inputs and outputs
- Structured errors
- Explicit authorization boundaries
- Capability-aware interfaces
- Hardware-independent service contracts
- Service availability and readiness states
- Interface versioning
- Independent service and interface testing
- Separation between service interfaces and user-facing presentation

The following decisions remain open:

- Exact service naming convention
- Exact interface, method, property, and signal names
- Final data schemas
- Final capability representation
- Asynchronous operation model
- Final authorization integration
- Concrete IPC mapping
- Final interface versioning mechanism

These decisions will be refined during service-interface and IPC prototyping.

## Service Interface Model Status

This document represents the initial logical service interface model for Stage 2.

It defines the contract principles that later implementation and IPC prototypes should follow.

The model is intentionally implementation-independent and will evolve as the Zethropol service architecture is validated.
