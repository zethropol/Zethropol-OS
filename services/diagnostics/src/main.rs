mod diagnostics_service;

use diagnostics_service::DiagnosticsService;
use std::time::Duration;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol Diagnostics Service başlatılıyor...");

    let service = DiagnosticsService;

    let _connection = zbus::connection::Builder::system()?
        .name("org.zethropol.Diagnostics")?
        .serve_at("/org/zethropol/Diagnostics", service)?
        .build()
        .await?;

    println!("Diagnostics D-Bus servisi aktif.");

    loop {
        tokio::time::sleep(Duration::from_secs(3600)).await;
    }
}
