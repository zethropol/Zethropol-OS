# Zethropol Packaging Architecture

## Purpose

Zethropol services are developed under services/ and packaged independently under packages/.

The service source tree owns service implementation and deployment definitions. The package tree owns Arch Linux package construction and installation rules.

## Repository layout

services/<service>/
- Cargo.toml
- Cargo.lock
- src/
- deployment/
  - dbus/
  - systemd/

packages/zethropol-<service>/
- README.md
- PKGBUILD

## Runtime installation

A production service package may install:

- service binaries under /usr/lib/zethropol/
- D-Bus policy files under /etc/dbus-1/system.d/
- D-Bus activation descriptors under /usr/share/dbus-1/system-services/
- systemd units under /usr/lib/systemd/system/

## Service activation

System-level Zethropol services use D-Bus activation integrated with systemd where appropriate.

A package must not enable an on-demand D-Bus service through multi-user.target merely to make it available.

## Build requirements

Package builds must not depend on developer-specific absolute paths or files under /home/<user>.

The final source acquisition model may use a release archive, package source archive, or another reproducible source mechanism established by the Zethropol build infrastructure.

## Scope

This document defines the repository-level packaging boundary. Individual packages may define additional dependencies, permissions, hardening, installation hooks, and service-specific requirements.
