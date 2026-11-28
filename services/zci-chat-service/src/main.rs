mod bridge;
mod config;
mod crypto_store;
mod dbus_service;
mod guard;

#[cfg(test)]
mod tests;

use config::{ChatConfig, CHAT_BUS_NAME, CHAT_OBJECT_PATH};
use dbus_service::ZciChatService;
use std::path::PathBuf;

struct LaunchOptions {
    session_bus_override: Option<String>,
    system_bus_override: Option<String>,
    config: ChatConfig,
}

fn parse_launch_options() -> LaunchOptions {
    let mut opts = LaunchOptions {
        session_bus_override: None,
        system_bus_override: None,
        config: ChatConfig::default(),
    };

    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0usize;
    while i < args.len() {
        match args[i].as_str() {
            "--private-bus" if i + 1 < args.len() => {
                // Connects both chat export & downstream lookups to an isolated private test bus.
                let addr = args[i + 1].clone();
                opts.session_bus_override = Some(addr.clone());
                opts.system_bus_override = Some(addr);
                i += 2;
            }
            "--session-bus" if i + 1 < args.len() => {
                opts.session_bus_override = Some(args[i + 1].clone());
                i += 2;
            }
            "--system-bus" if i + 1 < args.len() => {
                opts.system_bus_override = Some(args[i + 1].clone());
                i += 2;
            }
            "--storage-dir" if i + 1 < args.len() => {
                opts.config.storage_dir = PathBuf::from(&args[i + 1]);
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
    println!("Zethropol OS ZCI Chatbot Servisi ({CHAT_BUS_NAME}) başlatılıyor...");

    let opts = parse_launch_options();

    let system_conn =
        bridge::connect_downstream_bus(opts.system_bus_override.as_deref(), &opts.config).await?;

    let service = ZciChatService::new(opts.config, system_conn);

    let session_builder = match opts.session_bus_override {
        Some(ref addr) => zbus::connection::Builder::address(addr.as_str())?,
        None => zbus::connection::Builder::session()?,
    };

    let connection = session_builder
        .name(CHAT_BUS_NAME)?
        .serve_at(CHAT_OBJECT_PATH, service)?
        .build()
        .await?;

    println!("ZCI Chatbot D-Bus servisi kullanıcı oturumunda aktif ({CHAT_BUS_NAME}).");

    connection.closed().await;
    eprintln!("ZCI Chatbot D-Bus bağlantısı kapandı; çıkılıyor.");
    std::process::exit(1);
}
