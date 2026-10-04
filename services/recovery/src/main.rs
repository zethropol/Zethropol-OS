mod models;
mod recovery_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Recovery Manager başlatılıyor...");

    let recovery_mgr = recovery_service::RecoveryManager::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Recovery")?
        .serve_at("/org/zethropol/Recovery", recovery_mgr)?
        .build()
        .await?;

    println!("Recovery D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}