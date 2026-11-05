//! Resolve a real logind session for a bus-authenticated UID/PID.
//!
//! User-manager applications need not belong to a session scope. Only logind's
//! own per-user Display association may bridge that case; no UID, session or
//! environment supplied by the client is used.

use crate::broker_protocol::BrokerError;
use zbus::zvariant::OwnedObjectPath;

#[derive(Clone, Debug, PartialEq, Eq)]
struct SessionRef {
    id: String,
    path: OwnedObjectPath,
}

#[derive(Clone)]
struct UserInfo {
    uid: u32,
    display: SessionRef,
    sessions: Vec<SessionRef>,
}

#[derive(Clone)]
struct SessionInfo {
    id: String,
    uid: u32,
    active: bool,
    remote: bool,
    kind: String,
    class: String,
}

trait LoginAuthority {
    async fn session_for_pid(&self, pid: u32) -> Result<Option<OwnedObjectPath>, BrokerError>;
    async fn user_for_pid(&self, pid: u32) -> Result<UserInfo, BrokerError>;
    async fn session(&self, path: &OwnedObjectPath) -> Result<SessionInfo, BrokerError>;
}

fn lookup_error(error: zbus::Error) -> BrokerError {
    BrokerError::IdentityLookup(error.to_string())
}

fn no_session_for_pid(error: &zbus::Error) -> bool {
    matches!(error, zbus::Error::MethodError(name, _, _)
        if name.as_str() == "org.freedesktop.login1.NoSessionForPID")
}

struct Logind<'a> {
    connection: &'a zbus::Connection,
    manager: zbus::Proxy<'a>,
}

impl LoginAuthority for Logind<'_> {
    async fn session_for_pid(&self, pid: u32) -> Result<Option<OwnedObjectPath>, BrokerError> {
        match self.manager.call("GetSessionByPID", &(pid,)).await {
            Ok(path) => Ok(Some(path)),
            Err(error) if no_session_for_pid(&error) => Ok(None),
            Err(error) => Err(lookup_error(error)),
        }
    }

    async fn user_for_pid(&self, pid: u32) -> Result<UserInfo, BrokerError> {
        let path: OwnedObjectPath = self
            .manager
            .call("GetUserByPID", &(pid,))
            .await
            .map_err(lookup_error)?;
        let user = zbus::Proxy::new(
            self.connection,
            "org.freedesktop.login1",
            path,
            "org.freedesktop.login1.User",
        )
        .await
        .map_err(lookup_error)?;
        let uid = user.get_property("UID").await.map_err(lookup_error)?;
        let (id, path): (String, OwnedObjectPath) =
            user.get_property("Display").await.map_err(lookup_error)?;
        let sessions: Vec<(String, OwnedObjectPath)> =
            user.get_property("Sessions").await.map_err(lookup_error)?;
        Ok(UserInfo {
            uid,
            display: SessionRef { id, path },
            sessions: sessions
                .into_iter()
                .map(|(id, path)| SessionRef { id, path })
                .collect(),
        })
    }

    async fn session(&self, path: &OwnedObjectPath) -> Result<SessionInfo, BrokerError> {
        let session = zbus::Proxy::new(
            self.connection,
            "org.freedesktop.login1",
            path.clone(),
            "org.freedesktop.login1.Session",
        )
        .await
        .map_err(lookup_error)?;
        let (uid, _): (u32, OwnedObjectPath) =
            session.get_property("User").await.map_err(lookup_error)?;
        Ok(SessionInfo {
            id: session.get_property("Id").await.map_err(lookup_error)?,
            uid,
            active: session.get_property("Active").await.map_err(lookup_error)?,
            remote: session.get_property("Remote").await.map_err(lookup_error)?,
            kind: session.get_property("Type").await.map_err(lookup_error)?,
            class: session.get_property("Class").await.map_err(lookup_error)?,
        })
    }
}

fn verify_user(user: &UserInfo, uid: u32) -> Result<(), BrokerError> {
    if user.uid != uid
        || user.display.id.is_empty()
        || user.display.path.as_str() == "/"
        || !user.sessions.contains(&user.display)
    {
        return Err(BrokerError::InvalidIdentity);
    }
    Ok(())
}

async fn resolve_with(
    authority: &impl LoginAuthority,
    uid: u32,
    pid: u32,
) -> Result<String, BrokerError> {
    // PID 0 means the caller of logind, which would be the privileged broker.
    if pid == 0 {
        return Err(BrokerError::InvalidIdentity);
    }
    if let Some(path) = authority.session_for_pid(pid).await? {
        let session = authority.session(&path).await?;
        if session.uid != uid || session.id.is_empty() {
            return Err(BrokerError::InvalidIdentity);
        }
        return Ok(session.id);
    }

    // Only NoSessionForPID reaches here. Never turn a transport error into a
    // fallback and never select a session from an arbitrary UID or seat.
    let user = authority.user_for_pid(pid).await?;
    verify_user(&user, uid)?;
    let session = authority.session(&user.display.path).await?;
    if session.uid != uid
        || session.id != user.display.id
        || !session.active
        || session.remote
        || !matches!(session.kind.as_str(), "wayland" | "x11")
        || !matches!(
            session.class.as_str(),
            "user" | "user-early" | "user-light" | "user-early-light"
        )
    {
        return Err(BrokerError::InvalidIdentity);
    }
    // Detect a display switch or process/user removal during this lookup.
    let current = authority.user_for_pid(pid).await?;
    verify_user(&current, uid)?;
    if current.display != user.display {
        return Err(BrokerError::InvalidIdentity);
    }
    Ok(session.id)
}

pub(crate) async fn resolve(
    connection: &zbus::Connection,
    uid: u32,
    pid: u32,
) -> Result<String, BrokerError> {
    let manager = zbus::Proxy::new(
        connection,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
    )
    .await
    .map_err(lookup_error)?;
    resolve_with(
        &Logind {
            connection,
            manager,
        },
        uid,
        pid,
    )
    .await
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    struct MockLogin {
        direct: Result<Option<OwnedObjectPath>, BrokerError>,
        user: UserInfo,
        session: SessionInfo,
        user_calls: Cell<u32>,
        switch_display: bool,
    }

    fn reference(id: &str) -> SessionRef {
        SessionRef {
            id: id.into(),
            path: format!("/session/{id}").try_into().unwrap(),
        }
    }

    impl MockLogin {
        fn new() -> Self {
            Self {
                direct: Ok(None),
                user: UserInfo {
                    uid: 1000,
                    display: reference("s1"),
                    sessions: vec![reference("s1")],
                },
                session: SessionInfo {
                    id: "s1".into(),
                    uid: 1000,
                    active: true,
                    remote: false,
                    kind: "wayland".into(),
                    class: "user".into(),
                },
                user_calls: Cell::new(0),
                switch_display: false,
            }
        }
    }

    impl LoginAuthority for MockLogin {
        async fn session_for_pid(&self, pid: u32) -> Result<Option<OwnedObjectPath>, BrokerError> {
            assert_eq!(pid, 42);
            self.direct.clone()
        }
        async fn user_for_pid(&self, pid: u32) -> Result<UserInfo, BrokerError> {
            assert_eq!(pid, 42);
            self.user_calls.set(self.user_calls.get() + 1);
            let mut user = self.user.clone();
            if self.switch_display && self.user_calls.get() > 1 {
                user.display = reference("s2");
                user.sessions.push(user.display.clone());
            }
            Ok(user)
        }
        async fn session(&self, _: &OwnedObjectPath) -> Result<SessionInfo, BrokerError> {
            Ok(self.session.clone())
        }
    }

    #[tokio::test]
    async fn direct_session_is_bound_to_bus_uid_without_fallback() {
        let mut mock = MockLogin::new();
        mock.direct = Ok(Some(reference("s1").path));
        assert_eq!(resolve_with(&mock, 1000, 42).await.unwrap(), "s1");
        assert_eq!(mock.user_calls.get(), 0);
        assert_eq!(
            resolve_with(&mock, 2000, 42).await,
            Err(BrokerError::InvalidIdentity)
        );
    }

    #[tokio::test]
    async fn user_manager_uses_only_its_own_real_display_session() {
        for kind in ["wayland", "x11"] {
            let mut mock = MockLogin::new();
            mock.session.kind = kind.into();
            assert_eq!(resolve_with(&mock, 1000, 42).await.unwrap(), "s1");
            assert_eq!(mock.user_calls.get(), 2);
        }
    }

    #[tokio::test]
    async fn no_session_and_lingering_manager_never_invent_an_identity() {
        for missing in 0..3 {
            let mut mock = MockLogin::new();
            match missing {
                0 => mock.user.display.id.clear(),
                1 => mock.user.display.path = "/".try_into().unwrap(),
                _ => mock.user.sessions.clear(),
            }
            assert_eq!(
                resolve_with(&mock, 1000, 42).await,
                Err(BrokerError::InvalidIdentity)
            );
        }
    }

    #[tokio::test]
    async fn foreign_user_or_foreign_session_is_rejected() {
        for foreign in 0..2 {
            let mut mock = MockLogin::new();
            if foreign == 0 {
                mock.user.uid = 2000;
            } else {
                mock.session.uid = 2000;
            }
            assert_eq!(
                resolve_with(&mock, 1000, 42).await,
                Err(BrokerError::InvalidIdentity)
            );
        }
    }

    #[tokio::test]
    async fn inactive_remote_manager_and_non_graphical_displays_are_rejected() {
        for invalid in 0..5 {
            let mut mock = MockLogin::new();
            match invalid {
                0 => mock.session.active = false,
                1 => mock.session.remote = true,
                2 => mock.session.class = "manager".into(),
                3 => mock.session.kind = "tty".into(),
                _ => mock.session.id = "another-session".into(),
            }
            assert_eq!(
                resolve_with(&mock, 1000, 42).await,
                Err(BrokerError::InvalidIdentity)
            );
        }
    }

    #[tokio::test]
    async fn unavailable_logind_does_not_trigger_fallback() {
        let mut mock = MockLogin::new();
        mock.direct = Err(BrokerError::IdentityLookup(
            "access denied or disconnected".into(),
        ));
        assert!(matches!(
            resolve_with(&mock, 1000, 42).await,
            Err(BrokerError::IdentityLookup(_))
        ));
        assert_eq!(mock.user_calls.get(), 0);
    }

    #[tokio::test]
    async fn display_switch_during_lookup_is_rejected() {
        let mut mock = MockLogin::new();
        mock.switch_display = true;
        assert_eq!(
            resolve_with(&mock, 1000, 42).await,
            Err(BrokerError::InvalidIdentity)
        );
    }

    #[tokio::test]
    async fn pid_zero_never_resolves_the_privileged_broker_session() {
        assert_eq!(
            resolve_with(&MockLogin::new(), 1000, 0).await,
            Err(BrokerError::InvalidIdentity)
        );
    }

    #[test]
    fn only_the_explicit_no_session_error_allows_fallback() {
        for name in [
            "org.freedesktop.login1.NoSessionForPID",
            "org.freedesktop.DBus.Error.AccessDenied",
            "org.freedesktop.DBus.Error.NoReply",
            "org.freedesktop.login1.NoSuchUser",
        ] {
            let message = zbus::Message::method_call("/", "Test")
                .unwrap()
                .build(&())
                .unwrap();
            let error = zbus::Error::MethodError(name.try_into().unwrap(), None, message);
            assert_eq!(
                no_session_for_pid(&error),
                name == "org.freedesktop.login1.NoSessionForPID"
            );
        }
    }
}
