# Zethropol Control Center Module Map

## Purpose

This document defines the functional modules of Zethropol Control Center and their relationship with Zethropol services.

## Core Modules

- Overview
- Hardware
- Diagnostics
- Drivers
- Firmware
- Performance
- Power
- Updates
- Recovery
- Security
- Applications
- System Monitor

## Service Mapping

Overview → Zethropol services → normalized system state
Hardware → Zethropol Hardware Service → normalized hardware state
Diagnostics → Zethropol Hardware Service → diagnostic results
Drivers → Zethropol Hardware Service → driver state and compatibility
Firmware → Zethropol Hardware Service → firmware state
Performance → Zethropol services → performance capabilities and profiles
Power → Zethropol services → power state and profiles
Updates → Zethropol update service → update state and operations
Recovery → Zethropol recovery service → snapshots and rollback operations
Security → Zethropol security service → security state and controls
Applications → Zethropol software service → application management
System Monitor → Zethropol System Monitor → real-time system activity

## Access Model

Read modules primarily consume service state.
Observe modules consume service events where available.
Action modules request controlled service operations.
Administrative operations require explicit authorization.

## Architectural Rule

Control Center modules must communicate through the Zethropol API and must not directly access low-level Linux interfaces.

## Status

Initial module map for Phase 3 Desktop Experience.
