mod models;
mod power_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Power Service başlatılıyor...");

    let power_svc = power_service::PowerService::new();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Power")?
        .serve_at("/org/zethropol/Power", power_svc)?
        .build()
        .await?;

    println!("Power D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}
