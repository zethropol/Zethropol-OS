# Zethropol Control Center Technology

## Target Environment

Zethropol Control Center targets KDE Plasma 6 and Qt 6.

## UI Technology

The initial user-interface technology is Qt Quick and QML.

## Application Layer

The Control Center uses Qt/QML for presentation and interaction while communicating with Zethropol services through the defined service API.

## Service Layer

Zethropol services remain responsible for hardware access, system operations, diagnostics, privileged actions, and system state.

## Language Separation

Rust remains the primary implementation language for Zethropol system services.
QML and Qt are used for the user-facing desktop interface.

## Architectural Boundary

The Control Center must not directly access kernel interfaces, sysfs, procfs, hardware drivers, package managers, firmware utilities, or other privileged system interfaces.

## Compatibility

The implementation targets the Qt 6 and KDE Plasma environment used by Zethropol OS.

## Rationale

Qt Quick and QML provide the native UI technology required for a KDE Plasma desktop application while allowing the service architecture to remain independent of the presentation layer.

## Status

Initial technology decision for Zethropol Control Center.
