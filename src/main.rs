mod models;
mod security_service;

// Not: Mevcut mimarinizdeki diğer modüller
// mod hardware_service;
// mod performance_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("Zethropol OS Production Services başlatılıyor...");

    // Servislerin oluşturulması
    let security_svc = security_service::SecurityService::new();

    // Tek bir D-Bus bağlantısı üzerinden tüm servislerin nesne yollarına (Object Path) atanması
    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Security")?
        .serve_at("/org/zethropol/Security", security_svc)?
        .build()
        .await?;

    println!("Zethropol D-Bus servisleri aktif ve dinlemede.");

    // Servislerin arka planda sürekli çalışır durumda kalmasını sağla
    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(3600)).await;
    }
}
