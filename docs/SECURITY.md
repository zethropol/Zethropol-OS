# Zethropol OS — Security Architecture

## Purpose

This document defines the initial security architecture for Zethropol OS.

The security architecture is intended to protect system integrity, user data, privileged operations, and communication between Zethropol components while preserving the usability and maintainability of the operating system.

Security should be enforced through clear privilege boundaries, explicit authorization, validated interfaces, and reuse of established Linux security mechanisms.

## Security Principles

- Apply the principle of least privilege.
- Keep privileged operations isolated from user-facing components.
- Require explicit authorization for security-sensitive operations.
- Treat data received from user interfaces and external clients as untrusted input.
- Validate inputs before processing or applying changes.
- Prefer established Linux, systemd, D-Bus, and authorization mechanisms over custom security mechanisms.
- Keep security boundaries independent from the visual user interface.
- Fail safely when authorization, validation, or required security checks fail.
- Avoid unnecessary duplication of existing base-system security mechanisms.

## Privilege Model

Zethropol components should operate with the minimum privileges required for their responsibilities.

User-facing components should operate without administrative privileges by default.

Operations that modify system-wide state should be handled by a controlled privileged service boundary.

Privileged services should not expose unrestricted administrative access to user interfaces or external clients.

Privilege escalation should occur only through an explicit and authorized mechanism.

The privilege model should distinguish between:

- Unprivileged user operations
- Authorized administrative operations
- System-level service operations
- Hardware or kernel operations requiring elevated access

The exact privilege separation and service execution model will be refined during service and process architecture prototyping.

## Authorization

Authorization should be evaluated at the service boundary before privileged operations are performed.

Authorization decisions should be based on the requested operation, the requesting user or service, and the security policy applicable to that operation.

Zethropol should reuse established Linux authorization mechanisms where appropriate rather than introducing an independent authorization system without a clear architectural requirement.

Policy decisions and authorization failures should be represented through the structured service error model defined by the API architecture.

## User Interface Security

User-facing components must not directly access privileged system interfaces when a Zethropol service boundary is available.

The user interface should request operations through defined service APIs.

The service layer must validate requests and enforce authorization independently of the user interface.

A malicious or compromised user interface must not automatically gain unrestricted system privileges.

## Service Security

Zethropol services should expose only the interfaces required for their defined responsibilities.

Services should validate the identity, permissions, and parameters of requests before performing sensitive operations.

A service should not trust another component solely because it is part of the Zethropol system.

Inter-service communication should use defined interfaces and enforce the appropriate authorization policy.

Services should avoid exposing internal state, private files, or implementation-specific interfaces to unrelated components.

Service failures should not bypass security boundaries or leave privileged operations in an uncontrolled state.

## IPC Security

The selected inter-process communication mechanism must provide a security model suitable for system and user-session services.

D-Bus is the primary IPC candidate and its existing security and policy mechanisms should be evaluated as part of the final IPC decision.

IPC interfaces should distinguish between read-only operations and operations that can modify system state.

Sensitive operations should require explicit authorization at the service boundary.

The final IPC security policy will be defined after the IPC and process architecture prototypes are evaluated.

## Input Validation

All externally supplied configuration values, API parameters, and service requests must be treated as untrusted input.

Inputs should be validated for type, range, format, supported values, and required relationships before use.

Invalid input must produce a controlled error and must not result in partially applied privileged changes.

Validation should be performed by the component responsible for the operation rather than relying exclusively on user-interface validation.

## Data and File Access

Zethropol services should access only the files, devices, and system interfaces required for their responsibilities.

Access to sensitive system files should be minimized and should follow the service privilege boundary.

User data should not be accessed by system services unless required by an explicitly defined function.

Configuration files and service state should use appropriate filesystem permissions and ownership.

Temporary files and generated data should be handled in a way that prevents unintended disclosure or unauthorized modification.

## Secure State Changes

Security-sensitive operations should be designed to minimize the risk of partial or inconsistent system state.

Operations affecting multiple system resources should use transactional or rollback-capable mechanisms where practical.

A failed privileged operation should leave the system in a known and controlled state whenever possible.

Services should verify the resulting state after security-sensitive operations when the underlying system permits reliable verification.

## Error Handling and Failure Safety

Security failures must fail closed rather than silently bypassing authorization or validation.

Errors exposed to user-facing components should provide enough information to explain the failure without unnecessarily exposing sensitive implementation details.

Internal diagnostic information should remain available through appropriate logging and diagnostic mechanisms.

Security-related failures should not cause a service to continue operating with unintended privileges or invalid security state.

## Logging and Auditing

Security-relevant events should be observable through appropriate system logging mechanisms.

Events that may require investigation should include sufficient contextual information while avoiding unnecessary exposure of sensitive data.

Privileged operations, authorization failures, and significant security state changes should be considered for audit logging.

Logging mechanisms should integrate with existing Linux and systemd facilities where appropriate.

The final logging and auditing policy will be defined as part of the logging and observability architecture.

## Security Boundaries

Security boundaries should exist between:

- User-facing components and privileged services
- Zethropol services and underlying system interfaces
- Independent Zethropol services
- User sessions and system-wide services
- External clients and local system services

Each boundary should define what operations are permitted, what data may cross the boundary, and what authorization is required.

Security boundaries should be enforced by the service or system component that owns the protected resource.

## External Access

Zethropol services should not expose privileged system functionality to external clients by default.

Any future remote administration or external service access must use explicit authentication, authorization, and secure communication mechanisms.

External access should be disabled unless a defined feature requires it.

Remote access architecture will be defined separately if Zethropol introduces network-accessible management functionality.

## Base System Security

Zethropol should build upon the security mechanisms provided by Linux, systemd, D-Bus, the filesystem, and the CachyOS/Arch base system.

Existing security mechanisms should be configured and integrated rather than unnecessarily replaced.

Zethropol-specific security policies should complement the base system and remain within clearly defined architectural boundaries.

## Security Testing

Security-related components should be tested independently and as integrated system components.

Testing should cover:

- Unauthorized access attempts
- Insufficient permissions
- Invalid or malicious input
- Privileged operation failures
- Service isolation
- IPC authorization
- Configuration security
- Recovery from failed security-sensitive operations

Security testing should be included in system integration and release validation.

## Current Architectural Status

The following security principles are established:

- Least-privilege operation
- Explicit privilege boundaries
- Service-level authorization
- User-interface isolation from privileged operations
- Input validation
- Controlled security-sensitive state changes
- Failure-safe security behavior
- Restricted file and device access
- Security-aware IPC
- Integration with existing Linux security mechanisms

The following decisions remain open:

- Exact service execution privileges
- Final authorization mechanism and policy structure
- Final IPC security policy
- Detailed filesystem permissions and ownership
- Logging and audit policy
- Security hardening requirements
- External access and remote administration architecture

These decisions will be refined through service, process, IPC, and security prototyping.

## Security Architecture Status

This document represents the initial security architecture for Stage 2.

It is a working architectural document and will evolve as the Zethropol service and system security models are validated.
