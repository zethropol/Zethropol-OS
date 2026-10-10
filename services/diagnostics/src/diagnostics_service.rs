use serde_json::{Value, json};
use std::collections::HashMap;
use std::time::{SystemTime, UNIX_EPOCH};
use zbus::zvariant::OwnedValue;

pub struct DiagnosticsService;

fn operation_id() -> String {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|value| value.as_millis())
        .unwrap_or_default();

    format!("diagnostics-{timestamp}")
}

async fn inspect_service(
    connection: &zbus::Connection,
    service_name: &str,
    object_path: &str,
    interface_name: &str,
) -> Value {
    let result: Result<HashMap<String, OwnedValue>, String> = async {
        let proxy = zbus::Proxy::new(connection, service_name, object_path, interface_name)
            .await
            .map_err(|error| error.to_string())?;

        proxy
            .call("GetState", &())
            .await
            .map_err(|error| error.to_string())
    }
    .await;

    match result {
        Ok(state) => {
            let mut fields: Vec<String> = state.keys().cloned().collect();
            fields.sort();

            json!({
                "service": service_name,
                "reachable": true,
                "stateAvailable": true,
                "fields": fields
            })
        }
        Err(error) => json!({
            "service": service_name,
            "reachable": false,
            "stateAvailable": false,
            "fields": [],
            "error": error
        }),
    }
}

#[zbus::interface(name = "org.zethropol.Diagnostics.v1")]
impl DiagnosticsService {
    async fn get_report(&self) -> String {
        let id = operation_id();

        let report = match zbus::Connection::system().await {
            Ok(connection) => {
                let hardware = inspect_service(
                    &connection,
                    "org.zethropol.Hardware",
                    "/org/zethropol/Hardware",
                    "org.zethropol.Hardware.v1",
                )
                .await;

                let monitoring = inspect_service(
                    &connection,
                    "org.zethropol.Monitoring",
                    "/org/zethropol/Monitoring",
                    "org.zethropol.Monitoring.v1",
                )
                .await;

                json!({
                    "success": true,
                    "code": "report_generated",
                    "message": "Tanılama raporu oluşturuldu.",
                    "operationId": id,
                    "data": {
                        "diagnosticType": "service_health",
                        "checks": {
                            "hardware": hardware,
                            "monitoring": monitoring
                        }
                    }
                })
            }
            Err(error) => json!({
                "success": false,
                "code": "system_bus_unavailable",
                "message": "Sistem D-Bus bağlantısı kurulamadı.",
                "operationId": id,
                "data": {
                    "error": error.to_string()
                }
            }),
        };

        report.to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::operation_id;

    #[test]
    fn operation_id_uses_diagnostics_prefix() {
        let id = operation_id();
        assert!(id.starts_with("diagnostics-"));
        assert!(id.len() > "diagnostics-".len());
    }

    #[test]
    fn operation_ids_are_generated_for_each_operation() {
        let first = operation_id();
        let second = operation_id();
        assert!(first.starts_with("diagnostics-"));
        assert!(second.starts_with("diagnostics-"));
    }
}
