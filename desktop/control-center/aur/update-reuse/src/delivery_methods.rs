async fn prepare_aur_selection(
    &self,
    names: Vec<String>,
    versions: Vec<String>,
    #[zbus(connection)] connection: &zbus::Connection,
    #[zbus(header)] header: zbus::message::Header<'_>,
) -> Result<String, zbus::fdo::Error> {
    let review = approved_selection(&names, &versions).map_err(delivery_error)?;
    reserve_reviewed_aur_message(connection, &header, review)
        .await
        .map(|c| c.operation_id)
        .map_err(broker_error)
}
async fn get_aur_provisioning(
    &self,
    operation_id: String,
    #[zbus(connection)] connection: &zbus::Connection,
    #[zbus(header)] header: zbus::message::Header<'_>,
) -> Result<HashMap<String, String>, zbus::fdo::Error> {
    let context = delivery_context(connection, &header, &operation_id).await?;
    let request = tokio::task::spawn_blocking({
        let context = context.clone();
        move || provision_delivery(&context)
    })
    .await
    .map_err(delivery_error)?
    .map_err(delivery_error)?;
    Ok(HashMap::from([
        ("operation_id".into(), operation_id),
        ("request_json".into(), request),
        (
            "source_plan_sha256".into(),
            context.review.source_plan_sha256,
        ),
    ]))
}
async fn submit_aur_build(
    &self,
    operation_id: String,
    archives: Vec<zbus::zvariant::OwnedFd>,
    #[zbus(connection)] connection: &zbus::Connection,
    #[zbus(header)] header: zbus::message::Header<'_>,
) -> Result<(), zbus::fdo::Error> {
    let context = delivery_context(connection, &header, &operation_id).await?;
    tokio::task::spawn_blocking(move || submit_delivery(&context, archives))
        .await
        .map_err(delivery_error)?
        .map_err(delivery_error)
}
#[zbus(out_args(
    "receipt",
    "receipt_signature",
    "manifest",
    "manifest_signature",
    "archives",
    "signatures"
))]
async fn get_aur_attestation(
    &self,
    operation_id: String,
    #[zbus(connection)] connection: &zbus::Connection,
    #[zbus(header)] header: zbus::message::Header<'_>,
) -> Result<
    (
        zbus::zvariant::OwnedFd,
        zbus::zvariant::OwnedFd,
        zbus::zvariant::OwnedFd,
        zbus::zvariant::OwnedFd,
        Vec<zbus::zvariant::OwnedFd>,
        Vec<zbus::zvariant::OwnedFd>,
    ),
    zbus::fdo::Error,
> {
    let context = delivery_context(connection, &header, &operation_id).await?;
    tokio::task::spawn_blocking(move || delivery_attestation(&context))
        .await
        .map_err(delivery_error)?
        .map_err(delivery_error)
}
async fn release_aur_build(
    &self,
    operation_id: String,
    #[zbus(connection)] connection: &zbus::Connection,
    #[zbus(header)] header: zbus::message::Header<'_>,
) -> Result<(), zbus::fdo::Error> {
    let caller = AuthorizedCaller::check_message(
        connection,
        &header,
        PolicyAction::Preview,
        boottime_seconds()?,
    )
    .await
    .map_err(broker_error)?;
    persistent_aur_broker()
        .map_err(broker_error)?
        .release_aur_reservation(caller, &operation_id, boottime_seconds()?)
        .map_err(broker_error)?;
    tokio::task::spawn_blocking(move || provider_unit(&operation_id, false))
        .await
        .map_err(delivery_error)?
        .map_err(delivery_error)
}
