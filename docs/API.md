# Zethropol OS — Service API Architecture

## Purpose

This document defines the initial API model for communication between Zethropol services and user-facing components.

The API is designed to expose stable Zethropol concepts while keeping hardware-specific and implementation-specific details below the service interface.

This document defines the initial interface model. Exact D-Bus object paths, interface names, method signatures, data structures, and authorization rules remain subject to prototyping and testing.

## API Principles

- Expose stable Zethropol concepts rather than raw system interfaces.
- Keep hardware-specific implementation details below the API boundary.
- Separate read, observation, action, and administrative operations.
- Return structured data and structured errors.
- Keep privileged operations explicitly separated from read-only operations.
- Avoid exposing implementation details that higher layers do not need.
- Keep interfaces versionable and maintainable.

## Operation Categories

### Read

Read operations retrieve system or hardware state without modifying the system.

Examples include:

- Reading CPU information
- Reading GPU information
- Reading memory state
- Reading storage information
- Reading thermal state
- Reading power state

Read operations should normally be available without administrative privileges.

### Observe

Observe operations provide notifications when relevant system state changes.

Examples include:

- CPU or GPU state changes
- Thermal state changes
- Power or battery state changes
- Hardware availability changes
- Service state changes

Observation should use event-based notifications where practical rather than requiring constant polling by user interfaces.

### Action

Action operations request controlled system behavior that does not necessarily require unrestricted administrative privileges.

Examples include:

- Selecting a supported performance profile
- Requesting a diagnostic operation
- Requesting a controlled service operation

Each action should validate its input and current system state before execution.

### Admin

Admin operations can materially modify system state or require elevated privileges.

Examples include:

- Applying privileged configuration changes
- Performing system updates
- Starting a recovery or rollback operation
- Applying privileged performance or power changes

Admin operations must pass through an explicit authorization boundary.

## Service Interface Model

Each Zethropol service should expose a clearly defined interface.

An interface should describe:

- The information provided by the service
- The operations that can be requested
- The events that can be observed
- The accepted input data
- The returned data
- The possible errors
- The required authorization level

Interfaces should be designed around Zethropol concepts rather than directly exposing kernel, sysfs, procfs, or hardware-specific structures.

## Data Model

Service data should use stable and predictable structures.

Where practical, values should include sufficient context to be interpreted correctly.

Examples include:

- Numeric values with defined units
- Boolean state values
- Enumerated states
- Hardware identifiers
- Capability information
- Timestamps for time-sensitive observations

Services should avoid returning ambiguous values or implementation-specific representations when a normalized representation is possible.

## Error Model

Service errors should be structured and machine-readable.

An error should provide enough information for the caller to determine whether an operation failed because of:

- Invalid input
- Unsupported hardware or capability
- Missing system resources
- Insufficient authorization
- Conflicting system state
- Temporary service failure
- Underlying system failure

User-facing components may translate structured service errors into appropriate user-readable messages.

## IPC and D-Bus Model

D-Bus is currently the primary IPC candidate for Zethropol services.

If D-Bus is adopted, service interfaces should define stable bus names, object paths, interfaces, methods, properties, signals, and errors.

The exact naming scheme should be established before implementation begins.

The design should distinguish system-level services from user-session interfaces where appropriate.

Authorization policies should restrict access to privileged methods and service ownership where required.

## API Versioning

Zethropol service interfaces should be designed to evolve without unnecessarily breaking existing clients.

Breaking interface changes should result in a new interface version rather than silently changing the meaning of an existing operation.

Compatibility requirements should be evaluated before removing or changing an established interface.

## Security Requirements

Security is an architectural requirement of the service interface.

The API should follow the principle of least privilege.

In particular:

- Read-only interfaces should expose only necessary information.
- Privileged methods should require explicit authorization.
- Services should validate all external input.
- Services should not trust user-interface input as inherently safe.
- Sensitive operations should produce clear audit or diagnostic information where appropriate.

The detailed security and authorization architecture will be defined in a later stage.

## Current Architectural Status

The following API concepts are established:

- Read operations
- Observe operations
- Action operations
- Admin operations
- Structured data
- Structured errors
- Explicit authorization boundaries
- Hardware-independent service interfaces
- Versionable service interfaces

The following decisions remain open:

- Final IPC mechanism
- Exact D-Bus naming scheme
- Object and interface structure
- Method signatures
- Property definitions
- Signal definitions
- Detailed authorization policies
- API compatibility policy

These decisions will be refined through prototype implementation and testing.

## API Status

This document represents the initial service API architecture for Stage 2.

It is a working architectural document and is expected to evolve before production implementation.
