use crate::models::{OperationResult, UpdatePackageInfo, UpdateState};
use std::collections::HashMap;
use std::process::Command;
use std::sync::{Arc, Mutex};
use zbus::interface;

#[derive(Debug, Clone)]
pub struct OpState {
    pub status: String, // "pending", "running", "ok", "failed"
    pub message: String,
}

#[derive(Debug)]
pub struct UpdateManager {
    operations: Arc<Mutex<HashMap<String, OpState>>>,
}

impl UpdateManager {
    pub fn new() -> Self {
        Self {
            operations: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    fn check_pacman_updates(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        let output = Command::new("/usr/bin/checkupdates")
            .output()
            .map_err(|error| {
                zbus::fdo::Error::Failed(format!("Unable to start checkupdates: {error}"))
            })?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        parse_checkupdates_result(
            output.status.success(),
            output.status.code(),
            &stdout,
            &stderr,
        )
    }
}

#[interface(name = "org.zethropol.Update.v1")]
impl UpdateManager {
    async fn get_update_state(&self) -> Result<UpdateState, zbus::fdo::Error> {
        let pending = self.check_pacman_updates()?;
        let count = pending.len() as u32;

        Ok(UpdateState {
            is_checking: false,
            is_updating: false,
            last_check_time: "Just now".to_string(),
            pending_updates_count: count,
        })
    }

    async fn list_pending_updates(&self) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
        self.check_pacman_updates()
    }

    async fn get_operation_state(
        &self,
        operation_id: String,
    ) -> Result<HashMap<String, String>, zbus::fdo::Error> {
        let ops = self.operations.lock().unwrap();
        let mut res = HashMap::new();
        if let Some(state) = ops.get(&operation_id) {
            res.insert("status".to_string(), state.status.clone());
            res.insert("message".to_string(), state.message.clone());
        } else {
            res.insert("status".to_string(), "not-found".to_string());
            res.insert(
                "message".to_string(),
                "Operation ID does not exist.".to_string(),
            );
        }
        Ok(res)
    }

    async fn trigger_update(
        &self,
        #[zbus(header)] _header: zbus::message::Header<'_>,
    ) -> Result<OperationResult, zbus::fdo::Error> {
        Ok(OperationResult {
            success: false,
            code: "preflight_required".to_string(),
            message: "System updates are disabled until transaction planning, dependency/conflict validation, and authorization are implemented.".to_string(),
            operation_id: String::new(),
            data: HashMap::new(),
        })
    }

    #[zbus(signal)]
    async fn update_status_changed(
        ctxt: &zbus::object_server::SignalEmitter<'_>,
        new_state: UpdateState,
    ) -> zbus::Result<()>;
}
fn parse_checkupdates_result(
    success: bool,
    exit_code: Option<i32>,
    stdout: &str,
    stderr: &str,
) -> Result<Vec<UpdatePackageInfo>, zbus::fdo::Error> {
    if !success {
        if exit_code == Some(2) && stdout.trim().is_empty() {
            return Ok(Vec::new());
        }

        let detail = if stderr.trim().is_empty() {
            format!(
                "exit status: {}",
                exit_code
                    .map(|code| code.to_string())
                    .unwrap_or_else(|| "unknown".to_string())
            )
        } else {
            stderr.trim().to_string()
        };

        return Err(zbus::fdo::Error::Failed(format!(
            "Update check failed: {detail}"
        )));
    }

    Ok(parse_checkupdates_output(stdout))
}

fn parse_checkupdates_output(stdout: &str) -> Vec<UpdatePackageInfo> {
    stdout
        .lines()
        .filter_map(|line| {
            let parts: Vec<&str> = line.split_whitespace().collect();
            if parts.len() >= 4 && parts[2] == "->" {
                Some(UpdatePackageInfo {
                    package_name: parts[0].to_string(),
                    current_version: parts[1].to_string(),
                    new_version: parts[3].to_string(),
                    repository: "pacman".to_string(),
                    download_size: 0,
                })
            } else {
                None
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{parse_checkupdates_output, parse_checkupdates_result};

    #[test]
    fn parses_valid_update_lines() {
        let result = parse_checkupdates_output(
            "alpha 1.0 -> 1.1
beta 2.4-1 -> 3.0-1
",
        );
        assert_eq!(result.len(), 2);
        assert_eq!(result[0].package_name, "alpha");
        assert_eq!(result[0].current_version, "1.0");
        assert_eq!(result[0].new_version, "1.1");
        assert_eq!(result[0].repository, "pacman");
        assert_eq!(result[1].package_name, "beta");
        assert_eq!(result[1].new_version, "3.0-1");
    }

    #[test]
    fn ignores_empty_malformed_and_unrelated_lines() {
        let result = parse_checkupdates_output(
            "
not an update
alpha 1.0 1.1
short ->
",
        );
        assert!(result.is_empty());
    }

    #[test]
    fn accepts_empty_output() {
        assert!(parse_checkupdates_output("").is_empty());
    }

    #[test]
    fn treats_exit_code_two_with_empty_output_as_no_updates() {
        let result = parse_checkupdates_result(false, Some(2), "", "").unwrap();
        assert!(result.is_empty());
    }

    #[test]
    fn preserves_real_checkupdates_errors() {
        let result = parse_checkupdates_result(false, Some(1), "", "Cannot fetch updates");
        assert!(result.is_err());
    }
}
