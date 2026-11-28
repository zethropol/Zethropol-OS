mod actions;
mod config;
mod dbus_service;
mod policy_client;
mod sessions;

#[cfg(test)]
mod tests;

use config::{parse_loopback_gateway_addr, DaemonConfig, DAEMON_BUS_NAME, DAEMON_OBJECT_PATH};
use dbus_service::ZciDaemonService;
use std::path::PathBuf;

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
    // Fail safe: if euid cannot be verified, treat as privileged production mode (ignore test flags).
    true
}

struct LaunchOptions {
    private_bus: Option<String>,
    config: DaemonConfig,
}

fn parse_launch_options() -> LaunchOptions {
    let mut cfg = DaemonConfig::default();
    let _ = cfg.reload_from_file();
    let mut opts = LaunchOptions {
        private_bus: None,
        config: cfg,
    };

    // Security invariant: root production daemon NEVER accepts CLI path/bus overrides.
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
            "--model-gateway" if i + 1 < args.len() => {
                if let Ok(addr) = parse_loopback_gateway_addr(&args[i + 1]) {
                    opts.config.zci_chat_gateway = addr;
                    opts.config.zekans_gateway = addr;
                }
                i += 2;
            }
            "--runtime-dir" if i + 1 < args.len() => {
                opts.config.model_runtime_dir_override = Some(PathBuf::from(&args[i + 1]));
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
    println!("Zethropol OS ZCI Daemon (Policy-Gated Action Gateway) başlatılıyor...");

    let opts = parse_launch_options();
    let service = ZciDaemonService::with_config(opts.config);

    let builder = match opts.private_bus {
        Some(ref addr) => zbus::connection::Builder::address(addr.as_str())?,
        None => zbus::connection::Builder::system()?,
    };

    let connection = builder
        .name(DAEMON_BUS_NAME)?
        .serve_at(DAEMON_OBJECT_PATH, service)?
        .build()
        .await?;

    println!("ZCI Daemon D-Bus servisi aktif ve dinlemede ({DAEMON_BUS_NAME}).");

    connection.closed().await;
    eprintln!("ZCI Daemon D-Bus bağlantısı kapandı; servis yöneticisi kurtarması için çıkılıyor.");
    std::process::exit(1);
}
