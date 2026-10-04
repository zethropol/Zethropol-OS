# Zethropol Monitoring Package

Arch Linux package definition for the Zethropol Monitoring system service.

## Source

Service implementation:
services/monitoring/

Deployment definitions:
services/monitoring/deployment/

## Runtime components

- /usr/lib/zethropol/zethropol-monitoring
- /etc/dbus-1/system.d/org.zethropol.Monitoring.conf
- /usr/share/dbus-1/system-services/org.zethropol.Monitoring.service
- /usr/lib/systemd/system/zethropol-monitoring.service

## Package requirements

1. Build the Monitoring Rust service in release mode.
2. Install the service binary under /usr/lib/zethropol/.
3. Install the D-Bus policy under /etc/dbus-1/system.d/.
4. Install the D-Bus activation descriptor under /usr/share/dbus-1/system-services/.
5. Install the systemd unit under /usr/lib/systemd/system/.
6. Keep the service D-Bus activated and avoid enabling it through multi-user.target.
7. Provide a reproducible build without depending on the developer home directory.

The final PKGBUILD source strategy is intentionally deferred until the Zethropol package and release source model is established.
