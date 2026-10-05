# zethropol-performance

Zethropol Performance D-Bus service.

## D-Bus identity

- Bus name: org.zethropol.Performance
- Object path: /org/zethropol/Performance
- Interface: org.zethropol.Performance.v1

## Runtime

The service is D-Bus activated through systemd.

Binary:
- /usr/lib/zethropol/zethropol-performance

Deployment files:
- /etc/dbus-1/system.d/org.zethropol.Performance.conf
- /usr/share/dbus-1/system-services/org.zethropol.Performance.service
- /usr/lib/systemd/system/zethropol-performance.service
