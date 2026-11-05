// This entire include is cfg(test). It grants NO production authority.
pub(crate) mod aur_fixture {
    use super::*;
    pub(crate) struct Fixture {
        pub store: Arc<BrokerStore>,
        pub owner: VerifiedIdentity,
        pub path: PathBuf,
        cleanup: PathBuf,
    }
    impl Fixture {
        pub fn new() -> Self {
            let path = std::env::temp_dir().join(format!("aur-reuse-fixture-{}", Uuid::new_v4()));
            let store = BrokerStore::open_with(
                path.clone(),
                unsafe { libc::geteuid() },
                Arc::new(UuidTokenSource),
            )
            .unwrap();
            let owner = VerifiedIdentity::test_identity(
                ":1.400".into(),
                unsafe { libc::geteuid() },
                "fixture-session".into(),
            )
            .unwrap();
            Self {
                store: Arc::new(store),
                owner,
                cleanup: path.clone(),
                path,
            }
        }
        pub fn persistent(boot: &str, now: u64) -> Self {
            let mut fixture = Self::new();
            fixture.path = fixture.cleanup.join("durable");
            fixture.store = Arc::new(
                BrokerStore::open_aur_persistent_at(
                    fixture.path.clone(),
                    unsafe { libc::geteuid() },
                    boot,
                    now,
                    false,
                )
                .unwrap(),
            );
            fixture
        }
        pub fn reboot(&self, boot: &str, now: u64) -> Result<BrokerStore, BrokerError> {
            BrokerStore::open_aur_persistent_at(
                self.path.clone(),
                unsafe { libc::geteuid() },
                boot,
                now,
                false,
            )
        }
        pub fn queue(&self, plan: CanonicalPlan) -> ConsumedPreview {
            self.store
                .create_preview(
                    AuthorizedCaller::test_authorized(
                        self.owner.clone(),
                        PolicyAction::Preview,
                        100,
                    ),
                    plan.clone(),
                    100,
                )
                .unwrap();
            if self.store.aur_is_persistent().unwrap() {
                // Synthetic already-verified admission state for persistence
                // tests; no cryptographic or real PolicyKit claim is made.
                let id = self
                    .store
                    .read_record(&self.store.preview_path(&self.owner))
                    .unwrap()
                    .operation_id;
                let r = crate::update_service::ReviewedBuild {
                    source_package: plan.packages()[0].package_name.clone(),
                    source_version: plan.packages()[0].new_version.clone(),
                    architecture: "x86_64".into(),
                    source_plan_sha256: "a".repeat(64),
                    build_root_sha256: "b".repeat(64),
                    outputs: plan
                        .packages()
                        .iter()
                        .map(|p| crate::update_service::ReviewedOutput {
                            name: p.package_name.clone(),
                            version: p.new_version.clone(),
                            filename: p.filename.clone(),
                        })
                        .collect(),
                };
                let binding = AurReceiptBinding {
                    schema: 1,
                    operation_id: id.clone(),
                    receipt_sha256: plan.packages()[0].repository_metadata_sha256.clone(),
                    plan_hash: plan.hash(),
                    expires_at: 1000,
                    deadline: 400,
                    trust_sha256: "d".repeat(64),
                    signatures: plan
                        .packages()
                        .iter()
                        .map(|p| (p.package_name.clone(), "e".repeat(64)))
                        .collect(),
                    review: r,
                };
                self.store
                    .aur_create_companion(
                        &self.path.join(format!("aur-receipt-{id}.spent")),
                        &serde_json::to_vec(&binding).unwrap(),
                    )
                    .unwrap();
            }
            self.store
                .consume_approved_preview(
                    AuthorizedCaller::test_authorized(
                        self.owner.clone(),
                        PolicyAction::Install,
                        101,
                    ),
                    101,
                    &plan.hash(),
                    false,
                )
                .unwrap()
        }
        pub fn grant(
            &self,
            owner: VerifiedIdentity,
            action: PolicyAction,
            now: u64,
        ) -> AuthorizedCaller {
            AuthorizedCaller::test_authorized(owner, action, now)
        }
        pub fn other_peer(&self) -> VerifiedIdentity {
            VerifiedIdentity::test_identity(
                ":1.401".into(),
                unsafe { libc::geteuid() },
                "fixture-session".into(),
            )
            .unwrap()
        }
        pub fn reopen(&self) -> BrokerStore {
            BrokerStore::open_with(
                self.path.clone(),
                unsafe { libc::geteuid() },
                Arc::new(UuidTokenSource),
            )
            .unwrap()
        }
    }
    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.cleanup);
        }
    }
}
