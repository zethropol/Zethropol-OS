use std::fs;
use std::io::Write;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

fn unique_temp_dir(prefix: &str) -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let dir = std::env::temp_dir().join(format!("{prefix}-{}-{nanos}", std::process::id()));
    fs::create_dir_all(&dir).expect("failed to create temp dir for private dbus test");
    dir
}

#[test]
fn hardware_dbus_service_exits_cleanly_when_bus_disconnects() {
    let temp_dir = unique_temp_dir("zethropol-hw-dbus-disconnect");
    let sock_path = temp_dir.join("bus.sock");
    let addr = format!("unix:path={}", sock_path.display());
    let conf_path = temp_dir.join("bus.conf");

    let bus_conf = format!(
        r#"<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>{addr}</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
"#
    );
    let mut conf_file = fs::File::create(&conf_path).expect("failed to write bus.conf");
    conf_file
        .write_all(bus_conf.as_bytes())
        .expect("failed to flush bus.conf");

    let mut bus_proc = Command::new("dbus-daemon")
        .arg(format!("--config-file={}", conf_path.display()))
        .arg("--nofork")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .expect("failed to launch private dbus-daemon");

    let wait_sock = Instant::now();
    while !sock_path.exists() && wait_sock.elapsed() < Duration::from_secs(3) {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(sock_path.exists(), "private dbus socket was not created");

    let mut hw_proc = Command::new(env!("CARGO_BIN_EXE_hardware"))
        .arg("--dbus-service")
        .env("DBUS_SYSTEM_BUS_ADDRESS", &addr)
        .env("DBUS_SESSION_BUS_ADDRESS", &addr)
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .expect("failed to launch hardware --dbus-service");

    // Wait until org.zethropol.Hardware is registered on the private bus.
    let rt = tokio::runtime::Runtime::new().expect("failed to create tokio runtime");
    let registered = rt.block_on(async {
        let conn = zbus::connection::Builder::address(addr.as_str())
            .expect("invalid private bus address")
            .build()
            .await
            .expect("failed to connect test client to private bus");
        let proxy = zbus::fdo::DBusProxy::new(&conn)
            .await
            .expect("failed to create DBusProxy");
        let deadline = Instant::now() + Duration::from_secs(5);
        while Instant::now() < deadline {
            if proxy
                .name_has_owner("org.zethropol.Hardware".try_into().unwrap())
                .await
                .unwrap_or(false)
            {
                return true;
            }
            tokio::time::sleep(Duration::from_millis(25)).await;
        }
        false
    });
    assert!(registered, "hardware service did not register on private bus");

    // Abruptly terminate the private D-Bus daemon.
    bus_proc.kill().expect("failed to kill private dbus-daemon");
    let _ = bus_proc.wait();

    // Hardware service MUST detect transport closure and exit with code 1 (not hang forever).
    let exit_deadline = Instant::now() + Duration::from_secs(3);
    let mut exit_status = None;
    while Instant::now() < exit_deadline {
        if let Some(status) = hw_proc.try_wait().expect("failed to poll hardware process") {
            exit_status = Some(status);
            break;
        }
        std::thread::sleep(Duration::from_millis(25));
    }

    if exit_status.is_none() {
        let _ = hw_proc.kill();
        let _ = hw_proc.wait();
    }
    let _ = fs::remove_dir_all(&temp_dir);

    let status = exit_status.expect("hardware --dbus-service hung after D-Bus transport closed");
    assert_eq!(
        status.code(),
        Some(1),
        "expected exit code 1 on D-Bus transport disconnect"
    );
}
