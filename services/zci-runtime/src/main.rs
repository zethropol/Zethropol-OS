mod bridge;
mod config;
mod crypto_util;
mod dbus_service;
mod hardware;
mod lifecycle;
mod manifest;

#[cfg(test)]
mod tests;

use config::{parse_loopback_endpoint, RuntimeConfig, RUNTIME_BUS_NAME, RUNTIME_OBJECT_PATH};
use dbus_service::ZciRuntimeService;
use std::path::PathBuf;
use std::time::Duration;

fn is_root_euid() -> bool {
    if let Ok(status) = std::fs::read_to_string("/proc/self/status") {
        for line in status.lines() {
            if let Some(rest) = line.strip_prefix("Uid:") {
                let mut parts = rest.split_whitespace();
                let _real = parts.next();
                if let Some(effective) = parts.next() {
                    return effective == "0";
                }
            }
        }
    }
    true
}

struct LaunchOptions {
    private_bus: Option<String>,
    config: RuntimeConfig,
}

fn parse_launch_options() -> LaunchOptions {
    let mut cfg = RuntimeConfig::default();
    let _ = cfg.reload_from_file();
    let mut opts = LaunchOptions {
        private_bus: None,
        config: cfg,
    };

    // Security invariant: root production service NEVER accepts CLI path/bus overrides.
    if is_root_euid() {
        return opts;
    }

    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--private-bus" if i + 1 < args.len() => {
                opts.private_bus = Some(args[i + 1].clone());
                i += 2;
            }
            "--pkcheck-bin" if i + 1 < args.len() => {
                opts.config.pkcheck_bin = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--config-file" if i + 1 < args.len() => {
                opts.config.config_file = PathBuf::from(&args[i + 1]);
                let _ = opts.config.reload_from_file();
                i += 2;
            }
            "--manifest-file" if i + 1 < args.len() => {
                opts.config.manifest_file = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--engine-endpoint" if i + 1 < args.len() => {
                if let Ok(addr) = parse_loopback_endpoint(&args[i + 1]) {
                    opts.config.engine_endpoint = addr;
                }
                i += 2;
            }
            "--idle-sleep-secs" if i + 1 < args.len() => {
                if let Ok(secs) = args[i + 1].parse::<u64>() {
                    opts.config.idle_sleep_timeout_secs = secs;
                }
                i += 2;
            }
            "--hardware-override" if i + 1 < args.len() => {
                opts.config.hardware_override_file = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            _ => {
                i += 1;
            }
        }
    }
    opts.config.clamp_bounds();
    opts
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS ZCI Runtime (Yerel AI Çalışma Motoru) başlatılıyor...");

    let opts = parse_launch_options();
    let service = ZciRuntimeService::with_config(opts.config);
    let manager_handle = service.manager_handle();

    let builder = match opts.private_bus {
        Some(ref addr) => zbus::connection::Builder::address(addr.as_str())?,
        None => zbus::connection::Builder::system()?,
    };

    let connection = builder
        .name(RUNTIME_BUS_NAME)?
        .serve_at(RUNTIME_OBJECT_PATH, service)?
        .build()
        .await?;

    // Background passive standby idle-sleep monitor (11.02)
    let sweep_conn = connection.clone();
    tokio::spawn(async move {
        let mut ticker = tokio::time::interval(Duration::from_secs(1));
        loop {
            ticker.tick().await;
            let transitioned_model = manager_handle
                .lock()
                .ok()
                .and_then(|mut guard| guard.sweep_idle_standby());
            if let Some(model_id) = transitioned_model {
                if let Ok(iface_ref) = sweep_conn
                    .object_server()
                    .interface::<_, ZciRuntimeService>(RUNTIME_OBJECT_PATH)
                    .await
                {
                    let _ = ZciRuntimeService::runtime_state_changed(
                        iface_ref.signal_emitter(),
                        "Standby".to_string(),
                        model_id,
                        "idle-timeout-auto-sleep".to_string(),
                    )
                    .await;
                }
            }
        }
    });

    println!("ZCI Runtime D-Bus servisi aktif ve pasif beklemede ({RUNTIME_BUS_NAME}).");

    connection.closed().await;
    eprintln!("ZCI Runtime D-Bus bağlantısı kapandı; servis yöneticisi kurtarması için çıkılıyor.");
    std::process::exit(1);
}
