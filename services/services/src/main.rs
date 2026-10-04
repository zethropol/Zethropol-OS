mod models;
mod services_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Services Manager başlatılıyor...");

    let svc_manager = services_service::ServicesManager::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Services")?
        .serve_at("/org/zethropol/Services", svc_manager)?
        .build()
        .await?;

    println!("Services D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}