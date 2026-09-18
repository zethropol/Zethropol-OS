# Zethropol Control Center Architecture

## Purpose

Zethropol Control Center is the primary user-facing system management interface of Zethropol OS.

## Architectural Position

The Control Center operates above the Zethropol service layer and does not directly access low-level Linux interfaces.

## Architecture

User
  ↓
Zethropol Control Center
  ↓
Zethropol API
  ↓
Zethropol Services
  ↓
Linux subsystem

## Core Responsibilities

- Present system state
- Present hardware and capability information
- Present diagnostics and system health
- Provide controlled system actions
- Provide administrative operations through explicit authorization
- Present service and operation errors in user-readable form

## Data Flow

Read: GUI → API → Service → State
Observe: Service → API → GUI
Action: GUI → API → Service → Result
Admin: GUI → Authorization → API → Service → Result

## Hardware Integration

Hardware information is consumed through Zethropol Hardware Service and its normalized hardware state.

## System Monitor Integration

Real-time system activity may be consumed from the Zethropol System Monitor data source or an appropriate Zethropol service interface.

## Design Principle

The Control Center provides a unified interface without replacing the underlying Linux subsystems or duplicating their responsibilities.

## Architectural Constraint

User-interface components must not directly manipulate kernel, sysfs, procfs, hardware drivers, package managers, firmware tools, or other privileged system interfaces.

All system operations must pass through the appropriate Zethropol service boundary.
