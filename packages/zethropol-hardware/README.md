# Zethropol Hardware Package

This package builds and installs the production Zethropol Hardware D-Bus service.

## Runtime

The package installs:

- /usr/lib/zethropol/zethropol-hardware
- /etc/dbus-1/system.d/org.zethropol.Hardware.conf
- /usr/share/dbus-1/system-services/org.zethropol.Hardware.service
- /usr/lib/systemd/system/zethropol-hardware.service

The service uses system-level D-Bus activation through systemd.

The package does not enable the service through multi-user.target.

## Build

The package builds the Hardware service from the pinned Zethropol OS repository commit using Cargo.
