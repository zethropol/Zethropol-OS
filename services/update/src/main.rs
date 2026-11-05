mod boot_verifier;
mod broker_protocol;
mod cache_cleanup;
mod execution_evidence;
mod integrity;
mod models;
mod session_identity;
mod snapshot_policy;
mod update_service;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() == 3 && args[1] == "--worker" {
        return update_service::run_worker(&args[2]).map_err(Into::into);
    }
    if args.len() == 3 && args[1] == "--worker-stopped" {
        return update_service::mark_worker_interrupted(&args[2]).map_err(Into::into);
    }
    if args.len() != 1 {
        return Err("unexpected command line arguments".into());
    }
    println!("Zethropol OS Update Manager başlatılıyor...");

    let update_manager = update_service::UpdateManager::try_new()
        .map_err(|e| format!("update broker startup failed: {e:?}"))?;
    let broker = update_manager.broker_store();

    let _conn = zbus::connection::Builder::system()?
        .name("org.zethropol.Update")?
        .serve_at("/org/zethropol/Update", update_manager)?
        .build()
        .await?;

    println!("Update D-Bus servisi aktif ve dinlemede.");

    loop {
        tokio::time::sleep(tokio::time::Duration::from_secs(30)).await;
        if let Err(error) = update_service::reconcile_worker_records(&broker) {
            eprintln!("Worker state reconciliation unavailable: {error:?}");
        }
    }
}
