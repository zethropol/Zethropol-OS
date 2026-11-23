use std::collections::HashMap;
use std::fs;
use std::path::Path;
use std::process::Command;

use zbus::interface;
use zbus::object_server::SignalEmitter;
use zbus::zvariant::{OwnedValue, Str};

use crate::models::PowerMap;

const PKCHECK_BIN: &str = "/usr/bin/pkcheck";
const POWER_MODIFY_ACTION: &str = "org.zethropol.power.modify";
const ALLOWED_POWER_PROFILES: &[&str] = &["power-saver", "balanced", "performance"];
const MAX_GOVERNOR_LEN: usize = 32;

pub fn validate_power_profile(profile: &str) -> Result<(), &'static str> {
    if ALLOWED_POWER_PROFILES.contains(&profile) {
        Ok(())
    } else {
        Err("Invalid power profile")
    }
}

pub fn validate_governor(governor: &str) -> Result<(), &'static str> {
    if governor.is_empty()
        || governor.len() > MAX_GOVERNOR_LEN
        || !governor
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '_' || c == '-')
    {
        Err("invalid governor")
    } else {
        Ok(())
    }
}

pub struct PowerService;

impl PowerService {
    pub fn new() -> Self {
        Self
    }

    async fn authorize(&self, header: &zbus::message::Header<'_>) -> Result<(), zbus::fdo::Error> {
        let sender = header
            .sender()
            .ok_or_else(|| zbus::fdo::Error::Failed("invalid caller".to_string()))?
            .to_string();

        let authorized = tokio::task::spawn_blocking(move || {
            Command::new(PKCHECK_BIN)
                .args([
                    "--action-id",
                    POWER_MODIFY_ACTION,
                    "--system-bus-name",
                    &sender,
                    "--allow-user-interaction",
                ])
                .status()
        })
        .await
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?
        .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        if !authorized.success() {
            return Err(zbus::fdo::Error::AccessDenied(
                "Authentication required or permission denied".to_string(),
            ));
        }

        Ok(())
    }

    async fn power_profile(&self) -> Result<String, zbus::fdo::Error> {
        let connection = zbus::Connection::system()
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let reply = connection
            .call_method(
                Some("net.hadess.PowerProfiles"),
                "/net/hadess/PowerProfiles",
                Some("org.freedesktop.DBus.Properties"),
                "Get",
                &("net.hadess.PowerProfiles", "ActiveProfile"),
            )
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let value: OwnedValue = reply
            .body()
            .deserialize()
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let profile: String = value
            .try_into()
            .map_err(|_| zbus::fdo::Error::Failed("invalid active power profile".to_string()))?;

        Ok(profile)
    }

    async fn hardware_state(&self) -> Result<PowerMap, zbus::fdo::Error> {
        let connection = zbus::Connection::system()
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let reply = connection
            .call_method(
                Some("org.zethropol.Hardware"),
                "/org/zethropol/Hardware",
                Some("org.zethropol.Hardware.v1"),
                "GetState",
                &(),
            )
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let result: HashMap<String, OwnedValue> = reply
            .body()
            .deserialize()
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let data = result
            .get("data")
            .ok_or_else(|| zbus::fdo::Error::Failed("hardware data unavailable".to_string()))?;

        let data_map: HashMap<String, OwnedValue> = data
            .clone()
            .try_into()
            .map_err(|_| zbus::fdo::Error::Failed("invalid hardware data".to_string()))?;

        let power = data_map
            .get("power")
            .ok_or_else(|| zbus::fdo::Error::Failed("power state unavailable".to_string()))?;

        power
            .clone()
            .try_into()
            .map_err(|_| zbus::fdo::Error::Failed("invalid power state".to_string()))
    }

    async fn current_state(&self) -> Result<PowerMap, zbus::fdo::Error> {
        let mut power = self.hardware_state().await?;
        let profile = self
            .power_profile()
            .await
            .unwrap_or_else(|_| "unknown".to_string());
        power.insert(
            "profile".to_string(),
            OwnedValue::from(Str::from(profile.as_str())),
        );
        Ok(power)
    }

    async fn login1_call(&self, method: &str) -> Result<(), zbus::fdo::Error> {
        let connection = zbus::Connection::system()
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        connection
            .call_method(
                Some("org.freedesktop.login1"),
                "/org/freedesktop/login1",
                Some("org.freedesktop.login1.Manager"),
                method,
                &(true,),
            )
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        Ok(())
    }

    fn result(
        success: bool,
        code: &str,
        message: &str,
        operation_id: &str,
    ) -> HashMap<String, OwnedValue> {
        let mut result = HashMap::new();

        result.insert("success".to_string(), OwnedValue::from(success));
        result.insert("code".to_string(), OwnedValue::from(Str::from(code)));
        result.insert("message".to_string(), OwnedValue::from(Str::from(message)));
        result.insert(
            "operationId".to_string(),
            OwnedValue::from(Str::from(operation_id)),
        );

        result
    }

    fn operation_id() -> String {
        uuid::Uuid::new_v4().to_string()
    }

    fn apply_governor(governor: &str) -> Result<(), String> {
        validate_governor(governor).map_err(|e| e.to_string())?;

        let root = Path::new("/sys/devices/system/cpu/cpufreq");
        let entries = fs::read_dir(root).map_err(|error| error.to_string())?;

        let mut changed = false;

        for entry in entries.flatten() {
            let path = entry.path();

            if !path
                .file_name()
                .and_then(|name| name.to_str())
                .is_some_and(|name| name.starts_with("policy"))
            {
                continue;
            }

            let available =
                fs::read_to_string(path.join("scaling_available_governors")).unwrap_or_default();

            if !available.split_whitespace().any(|value| value == governor) {
                continue;
            }

            fs::write(path.join("scaling_governor"), governor)
                .map_err(|error| error.to_string())?;

            changed = true;
        }

        if changed {
            Ok(())
        } else {
            Err("requested governor is not available".to_string())
        }
    }
}

#[interface(name = "org.zethropol.Power.v1")]
impl PowerService {
    #[zbus(signal)]
    async fn state_changed(emitter: &SignalEmitter<'_>, state: PowerMap) -> zbus::Result<()>;

    async fn get_state(&self) -> Result<PowerMap, zbus::fdo::Error> {
        let power = self.current_state().await?;
        let mut result = Self::result(true, "ok", "", "");
        result.insert("data".to_string(), OwnedValue::from(power));
        Ok(result)
    }

    async fn set_profile(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        profile: &str,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        if let Err(msg) = validate_power_profile(profile) {
            return Ok(Self::result(false, "invalid-profile", msg, ""));
        }

        self.authorize(&header).await?;

        let operation_id = Self::operation_id();

        let connection = zbus::Connection::system()
            .await
            .map_err(|error| zbus::fdo::Error::Failed(error.to_string()))?;

        let value = zbus::zvariant::Value::from(profile);

        let result = connection
            .call_method(
                Some("net.hadess.PowerProfiles"),
                "/net/hadess/PowerProfiles",
                Some("org.freedesktop.DBus.Properties"),
                "Set",
                &("net.hadess.PowerProfiles", "ActiveProfile", value),
            )
            .await;

        match result {
            Ok(_) => {
                if let Ok(state) = self.current_state().await {
                    let _ = Self::state_changed(&emitter, state).await;
                }
                Ok(Self::result(
                    true,
                    "ok",
                    "Power profile changed",
                    &operation_id,
                ))
            }
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error.to_string(),
                &operation_id,
            )),
        }
    }

    async fn set_governor(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
        governor: &str,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        if let Err(msg) = validate_governor(governor) {
            return Ok(Self::result(false, "invalid-governor", msg, ""));
        }

        self.authorize(&header).await?;

        let operation_id = Self::operation_id();

        match Self::apply_governor(governor) {
            Ok(()) => {
                if let Ok(state) = self.current_state().await {
                    let _ = Self::state_changed(&emitter, state).await;
                }
                Ok(Self::result(
                    true,
                    "ok",
                    "CPU governor changed",
                    &operation_id,
                ))
            }
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error,
                &operation_id,
            )),
        }
    }

    async fn suspend(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        self.authorize(&header).await?;
        let operation_id = Self::operation_id();

        match self.login1_call("Suspend").await {
            Ok(()) => Ok(Self::result(true, "ok", "Suspend requested", &operation_id)),
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error.to_string(),
                &operation_id,
            )),
        }
    }

    async fn hibernate(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        self.authorize(&header).await?;
        let operation_id = Self::operation_id();

        match self.login1_call("Hibernate").await {
            Ok(()) => Ok(Self::result(
                true,
                "ok",
                "Hibernate requested",
                &operation_id,
            )),
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error.to_string(),
                &operation_id,
            )),
        }
    }

    async fn reboot(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        self.authorize(&header).await?;
        let operation_id = Self::operation_id();

        match self.login1_call("Reboot").await {
            Ok(()) => Ok(Self::result(true, "ok", "Reboot requested", &operation_id)),
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error.to_string(),
                &operation_id,
            )),
        }
    }

    async fn shutdown(
        &self,
        #[zbus(header)] header: zbus::message::Header<'_>,
    ) -> Result<PowerMap, zbus::fdo::Error> {
        self.authorize(&header).await?;
        let operation_id = Self::operation_id();

        match self.login1_call("PowerOff").await {
            Ok(()) => Ok(Self::result(
                true,
                "ok",
                "Shutdown requested",
                &operation_id,
            )),
            Err(error) => Ok(Self::result(
                false,
                "operation-failed",
                &error.to_string(),
                &operation_id,
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn power_profile_allowlist_accepts_known_and_rejects_arbitrary() {
        for ok in ["power-saver", "balanced", "performance"] {
            assert!(validate_power_profile(ok).is_ok());
        }
        for bad in [
            "",
            "balanced ",
            "balanced\n",
            "turbo",
            "../balanced",
            "performance;id",
            "BALANCED",
        ] {
            assert!(validate_power_profile(bad).is_err(), "expected err for {bad:?}");
        }
    }

    #[test]
    fn governor_validation_rejects_invalid_inputs() {
        assert!(validate_governor("schedutil").is_ok());
        assert!(validate_governor("powersave").is_ok());
        assert!(validate_governor("performance").is_ok());
        for bad in ["", "sched util", "../bad", "BAD", "a;b", "x\0y"] {
            assert!(validate_governor(bad).is_err(), "expected err for {bad:?}");
        }
    }
}
