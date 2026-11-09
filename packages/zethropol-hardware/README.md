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


## Hardware inventory revision

Release `0.1.0-2` pins the verified Hardware inventory implementation at
`cd976ad79a930f3a28e0b6b6184f047e979ba5f0`; `check()` runs its locked Rust tests.
The former `0.1.0-1` recipe pinned `3a8e40c` and would reinstall the older service.

The runtime contract remains `org.zethropol.Hardware`, `/org/zethropol/Hardware`,
`org.zethropol.Hardware.v1.GetState` with the standard `a{sv}` result envelope.
The Control Center build uses that same system bus contract directly. No separate
Hardware launcher, private production bus, or Control Center system installation
is required. The unit, activation file and policy paths are unchanged.

Before upgrading the installed Hardware package, preserve its actual binary,
unit, activation file, policy and package version for rollback. The package
upgrade and restart of only `zethropol-hardware.service` require explicit user
approval. Validate the system-bus result and its existing clients after restart;
restore the preserved package/files and Hardware service on failure.
