mod dbus;

fn main() {
    let runtime = tokio::runtime::Runtime::new()
        .expect("failed to create Tokio runtime");

    runtime.block_on(async {
        let service = dbus::PerformanceService;

        let _connection = zbus::connection::Builder::system()
            .expect("failed to connect to system D-Bus")
            .name("org.zethropol.Performance")
            .expect("failed to acquire D-Bus service name")
            .serve_at("/org/zethropol/Performance", service)
            .expect("failed to register Performance service")
            .build()
            .await
            .expect("failed to build Performance D-Bus service connection");

        std::future::pending::<()>().await;
    });
}
