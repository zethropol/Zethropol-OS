mod models;
mod update_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Update Manager başlatılıyor...");

    let update_manager = update_service::UpdateManager::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Update")?
        .serve_at("/org/zethropol/Update", update_manager)?
        .build()
        .await?;

    println!("Update D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}