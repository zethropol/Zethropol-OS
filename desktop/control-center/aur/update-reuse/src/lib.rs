// Shared Update implementation with a separately staged, non-activated candidate entry point.
#![allow(dead_code)]
macro_rules! reuse {
    ($name:ident) => {
        mod $name {
            include!(concat!(env!("OUT_DIR"), "/", stringify!($name), ".rs"));
        }
    };
}
reuse!(boot_verifier);
reuse!(cache_cleanup);
reuse!(broker_protocol);
reuse!(execution_evidence);
reuse!(integrity);
reuse!(models);
reuse!(session_identity);
reuse!(snapshot_policy);
reuse!(update_service);

include!(concat!(env!("OUT_DIR"), "/entry.rs"));
