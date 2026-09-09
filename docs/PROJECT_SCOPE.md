# Zethropol OS — Project Scope

## Purpose

Zethropol OS is intended to be a complete, user-installable Linux operating system built on CachyOS and Arch Linux.

The project focuses on turning a powerful and flexible Linux foundation into a coherent operating system experience that is practical for everyday users while remaining transparent and controllable for advanced users.

## Scope

Zethropol is intended to develop beyond a customized desktop appearance. The project may include system-level components, hardware-aware behavior, user-facing tools, recovery mechanisms, software management, diagnostics, and other infrastructure required to provide a coherent operating system experience.

The project should reuse established open-source technologies whenever they provide a suitable foundation. Zethropol-specific development should be reserved for functionality that provides meaningful value to the project.

## Intended Areas

The long-term project scope includes:

- System identity and configuration
- System architecture
- Hardware detection and hardware-aware behavior
- Performance and power management
- System monitoring and diagnostics
- Zethropol Control Center
- Safe update and recovery mechanisms
- Snapshot and rollback support
- Software management
- Security and system protection
- Gaming and creator-oriented capabilities
- Backup and maintenance tools
- Installer and release infrastructure
- Multi-hardware compatibility and testing

These areas represent the intended scope of the project and do not imply that the corresponding functionality is currently implemented.

## Desktop Environment

KDE Plasma is the current desktop foundation of Zethropol OS.

KDE Plasma should remain the default desktop environment unless a future architectural decision establishes a different direction.

The default choice does not prevent Zethropol from supporting alternative desktop environments where technically appropriate.

## Current Scope

At the current development stage, work is intentionally limited to the project foundation.

Current work includes:

- System identity
- Project organization
- Repository structure
- Visual foundation
- Development principles
- Foundational documentation

The project has not yet entered full system architecture development.

## Outside the Current Scope

The following areas are not currently considered active implementation stages:

- Full Zethropol Control Center
- Hardware intelligence framework
- Performance profile system
- Safe Update system
- Recovery and rollback system
- Final software management layer
- Final installer and ISO infrastructure
- Broad hardware validation

These areas may be addressed in later development stages after the required architecture has been defined.

## Scope Discipline

New functionality should not be added solely because it is technically possible.

Before introducing a major component, the project should establish:

1. The problem it solves.
2. Whether an existing open-source solution already solves the problem adequately.
3. Whether integration is preferable to developing a new component.
4. The maintenance and testing requirements it introduces.
5. Its compatibility with the overall Zethropol architecture.

This approach is intended to prevent unnecessary complexity and uncontrolled project expansion.

## Project Status

This document defines the intended scope and current boundaries of Zethropol OS. It is a planning and architectural reference, not a list of completed features.
