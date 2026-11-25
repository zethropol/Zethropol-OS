mod dbus;
mod telemetry;

use std::sync::Arc;
use tokio::sync::RwLock;

use dbus::{
    BusTarget, DbusHardwareClient, MONITORING_PATH, MONITORING_SERVICE, MonitoringService,
    SamplerConfig,
};
use telemetry::TelemetryCache;

fn main() {
    let runtime = tokio::runtime::Runtime::new().expect("failed to create Tokio runtime");

    runtime.block_on(async {
        let config = SamplerConfig::default();
        let cache = Arc::new(RwLock::new(TelemetryCache::new(config.max_sample_age)));
        let service = MonitoringService {
            cache: Arc::clone(&cache),
        };

        let connection = zbus::connection::Builder::system()
            .expect("failed to connect to system D-Bus")
            .name(MONITORING_SERVICE)
            .expect("failed to acquire D-Bus service name")
            .serve_at(MONITORING_PATH, service)
            .expect("failed to register Monitoring service")
            .build()
            .await
            .expect("failed to build Monitoring D-Bus service connection");

        let hw_client = DbusHardwareClient::from_shared_connection(
            connection.clone(),
            BusTarget::System,
        );

        let sampler = tokio::spawn(dbus::run_sampler(
            hw_client,
            cache,
            config,
            connection.clone(),
        ));

        // Exit cleanly with non-zero status if the D-Bus transport drops so
        // systemd (`BindsTo=dbus.socket`, `Restart=on-failure`) re-establishes
        // the bus name and object server cleanly.
        connection.closed().await;
        sampler.abort();
        eprintln!("Monitoring D-Bus connection closed; exiting for service manager recovery.");
        std::process::exit(1);
    });
}
