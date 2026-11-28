mod audit_log;
mod crypto_util;
mod models;
mod policy_dbus;
mod policy_engine;
mod policy_state;
mod security_service;
mod untrusted;

#[cfg(test)]
mod policy_tests;

use policy_dbus::{PolicyDbusService, DEFAULT_PKCHECK_BIN, POLICY_DBUS_PATH};
use policy_engine::PolicyConfig;
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
    policy_config: PolicyConfig,
    pkcheck_bin: PathBuf,
}

fn parse_launch_options() -> LaunchOptions {
    let mut opts = LaunchOptions {
        private_bus: None,
        policy_config: PolicyConfig::default(),
        pkcheck_bin: PathBuf::from(DEFAULT_PKCHECK_BIN),
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
            "--state-file" if i + 1 < args.len() => {
                opts.policy_config.state_file = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--audit-log" if i + 1 < args.len() => {
                opts.policy_config.audit_log_file = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            "--home-override" if i + 1 < args.len() => {
                opts.policy_config.home_override = Some(PathBuf::from(&args[i + 1]));
                i += 2;
            }
            "--pkcheck-bin" if i + 1 < args.len() => {
                opts.pkcheck_bin = PathBuf::from(&args[i + 1]);
                i += 2;
            }
            _ => {
                i += 1;
            }
        }
    }
    opts
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Security & ZCI Policy Service başlatılıyor...");

    let opts = parse_launch_options();
    let security_svc = security_service::SecurityService::new();
    let policy_svc = PolicyDbusService::with_config(opts.policy_config, opts.pkcheck_bin);

    let builder = match opts.private_bus {
        Some(ref addr) => zbus::connection::Builder::address(addr.as_str())?,
        None => zbus::connection::Builder::system()?,
    };

    let connection = builder
        .name("org.zethropol.Security")?
        .serve_at("/org/zethropol/Security", security_svc)?
        .serve_at(POLICY_DBUS_PATH, policy_svc)?
        .build()
        .await?;

    println!("Security & ZCI Policy D-Bus servisi aktif ve dinlemede.");

    connection.closed().await;
    eprintln!("Security D-Bus bağlantısı kapandı; servis yöneticisi kurtarması için çıkılıyor.");
    std::process::exit(1);
}
