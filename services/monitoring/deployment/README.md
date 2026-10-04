# Monitoring Service Deployment

Runtime installation layout:

- Binary: /usr/lib/zethropol/zethropol-monitoring
- D-Bus policy: /etc/dbus-1/system.d/org.zethropol.Monitoring.conf
- D-Bus activation: /usr/share/dbus-1/system-services/org.zethropol.Monitoring.service
- systemd unit: /usr/lib/systemd/system/zethropol-monitoring.service

The Cargo release binary is produced as target/release/monitoring and is installed as zethropol-monitoring.
