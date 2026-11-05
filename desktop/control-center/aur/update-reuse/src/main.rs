// Release candidate only; staging never launches or installs this process.
#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    aur_update_reuse::run_service().await
}
