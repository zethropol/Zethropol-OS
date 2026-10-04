mod models;
mod security_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Security Service başlatılıyor...");

    let security_svc = security_service::SecurityService::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Security")?
        .serve_at("/org/zethropol/Security", security_svc)?
        .build()
        .await?;

    println!("Security D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}
