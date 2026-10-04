mod dbus;

use std::sync::Arc;
use tokio::sync::RwLock;

fn main() {
    let runtime = tokio::runtime::Runtime::new()
        .expect("failed to create Tokio runtime");

    runtime.block_on(async {
        let state = Arc::new(RwLock::new(None));
        let sampler_state = Arc::clone(&state);

        tokio::spawn(async move {
            dbus::run_sampler(sampler_state).await;
        });

        let service = dbus::MonitoringService { state };

        let _connection = zbus::connection::Builder::system()
            .expect("failed to connect to system D-Bus")
            .name("org.zethropol.Monitoring")
            .expect("failed to acquire D-Bus service name")
            .serve_at("/org/zethropol/Monitoring", service)
            .expect("failed to register Monitoring service")
            .build()
            .await
            .expect("failed to build Monitoring D-Bus service connection");

        std::future::pending::<()>().await;
    });
}
