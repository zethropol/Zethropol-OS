# zethropol-services

Arch Linux package definition for the Zethropol Services D-Bus service.

## Runtime

Installs:

- /usr/lib/zethropol/zethropol-services
- /etc/dbus-1/system.d/org.zethropol.Services.conf
- /usr/share/dbus-1/system-services/org.zethropol.Services.service
- /usr/lib/systemd/system/zethropol-services.service

The service uses D-Bus activation through systemd and is not enabled through multi-user.target.
