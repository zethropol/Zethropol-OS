mod models;
mod applications_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Applications Manager başlatılıyor...");

    let app_manager = applications_service::ApplicationsManager::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Applications")?
        .serve_at("/org/zethropol/Applications", app_manager)?
        .build()
        .await?;

    println!("Applications D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}