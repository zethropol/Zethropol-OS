mod audit_store;
mod bridge;
mod config;
mod dbus_service;
mod planner;
#[cfg(test)]
mod tests;

use config::{AgentConfig, AGENT_BUS_NAME, AGENT_OBJECT_PATH};
use dbus_service::ZciAgentService;
use std::env;
use std::error::Error;
use std::path::PathBuf;

struct CliOptions {
    private_bus: Option<String>,
    storage_dir: Option<PathBuf>,
    home_override: Option<PathBuf>,
    extra_workspaces: Vec<PathBuf>,
}

fn parse_cli_args() -> Result<CliOptions, String> {
    let mut opts = CliOptions {
        private_bus: None,
        storage_dir: None,
        home_override: None,
        extra_workspaces: Vec::new(),
    };
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--private-bus" => {
                let val = args
                    .next()
                    .ok_or_else(|| "--private-bus requires a D-Bus address".to_string())?;
                opts.private_bus = Some(val);
            }
            "--storage-dir" => {
                let val = args
                    .next()
                    .ok_or_else(|| "--storage-dir requires a directory path".to_string())?;
                opts.storage_dir = Some(PathBuf::from(val));
            }
            "--home-override" => {
                let val = args
                    .next()
                    .ok_or_else(|| "--home-override requires a directory path".to_string())?;
                opts.home_override = Some(PathBuf::from(val));
            }
            "--extra-workspace" => {
                let val = args
                    .next()
                    .ok_or_else(|| "--extra-workspace requires a directory path".to_string())?;
                opts.extra_workspaces.push(PathBuf::from(val));
            }
            "--help" | "-h" => {
                println!(
                    "zethropol-zci-agentd [--private-bus <addr>] [--storage-dir <dir>] [--home-override <dir>] [--extra-workspace <dir>]"
                );
                std::process::exit(0);
            }
            other => {
                return Err(format!("Unknown argument: {other}"));
            }
        }
    }
    Ok(opts)
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn Error>> {
    let opts = parse_cli_args().map_err(|e| format!("CLI error: {e}"))?;
    let mut cfg = AgentConfig::default();
    if let Some(dir) = opts.storage_dir {
        cfg.storage_dir = dir;
    }
    if let Some(home) = opts.home_override {
        cfg.home_dir = home;
    }
    cfg.extra_workspace_roots = opts.extra_workspaces;

    let (serve_conn, system_conn) = if let Some(addr) = opts.private_bus {
        let sys = zbus::connection::Builder::address(addr.as_str())?
            .build()
            .await?;
        let srv = zbus::connection::Builder::address(addr.as_str())?
            .name(AGENT_BUS_NAME)?
            .serve_at(AGENT_OBJECT_PATH, ZciAgentService::new(cfg, sys.clone()))?
            .build()
            .await?;
        (srv, sys)
    } else {
        let sys = zbus::Connection::system().await?;
        let srv = zbus::connection::Builder::session()?
            .name(AGENT_BUS_NAME)?
            .serve_at(AGENT_OBJECT_PATH, ZciAgentService::new(cfg, sys.clone()))?
            .build()
            .await?;
        (srv, sys)
    };

    let _keep_sys = system_conn;
    serve_conn.closed().await;
    Ok(())
}
