//! The engine's session token file (`docs/remote-control.md`, "Session
//! token").
//!
//! The engine always requires a token. Without `QUAT_REMOTE_TOKEN` it
//! generates one per run and writes it to a user-only file whose path depends
//! only on the port, so quat-mcp can find it with no configuration:
//!
//! | Platform | Path |
//! |---|---|
//! | Windows | `%LOCALAPPDATA%\quat-3d\remote-<port>.token` |
//! | macOS | `~/Library/Application Support/quat-3d/remote-<port>.token` |
//! | Linux | `$XDG_RUNTIME_DIR/quat-3d/remote-<port>.token`, else `$XDG_STATE_HOME` (default `~/.local/state`) |
//!
//! `QUAT_REMOTE_TOKEN_FILE` overrides the path on both sides. The file holds
//! the token on one line. It is read again on every new connection, because
//! the token changes whenever the engine restarts.

use std::io;
use std::path::{Path, PathBuf};

use tokio::io::AsyncReadExt;

/// Longest token accepted from the file (the engine writes 64 hex digits).
pub const MAX_TOKEN_BYTES: u64 = 4096;

/// Why the token file could not be used.
#[derive(Debug, thiserror::Error)]
pub enum TokenFileError {
    #[error("cannot read {}: {source}", path.display())]
    Io { path: PathBuf, source: io::Error },
    #[error("{} is empty", path.display())]
    Empty { path: PathBuf },
    #[error("{} does not hold a token (expected one line of printable ASCII, at most {MAX_TOKEN_BYTES} bytes)", path.display())]
    Invalid { path: PathBuf },
}

/// The token file of an engine listening on `port`, or `None` when the
/// platform has no per-user directory for it (no home directory).
#[must_use]
pub fn default_path(port: u16) -> Option<PathBuf> {
    let dir = if cfg!(any(windows, target_os = "macos")) {
        dirs::data_local_dir()
    } else {
        dirs::runtime_dir().or_else(dirs::state_dir)
    };
    Some(dir?.join("quat-3d").join(format!("remote-{port}.token")))
}

/// Reads the token from `path`: `Ok(None)` when the file does not exist (the
/// engine is not running, or runs on another port or an older build).
///
/// # Errors
/// The file exists but cannot be read, or does not hold a single token.
pub async fn read(path: &Path) -> Result<Option<String>, TokenFileError> {
    let io_err = |source| TokenFileError::Io {
        path: path.to_owned(),
        source,
    };
    let file = match tokio::fs::File::open(path).await {
        Ok(f) => f,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(None),
        Err(e) => return Err(io_err(e)),
    };
    let mut bytes = Vec::new();
    file.take(MAX_TOKEN_BYTES + 1)
        .read_to_end(&mut bytes)
        .await
        .map_err(io_err)?;
    let invalid = || TokenFileError::Invalid {
        path: path.to_owned(),
    };
    if u64::try_from(bytes.len()).map_or(true, |n| n > MAX_TOKEN_BYTES) {
        return Err(invalid());
    }
    let text = std::str::from_utf8(&bytes).map_err(|_| invalid())?;
    let token = text.trim_end_matches(['\r', '\n']);
    if token.is_empty() {
        return Err(TokenFileError::Empty {
            path: path.to_owned(),
        });
    }
    if !token.bytes().all(|b| b.is_ascii_graphic()) {
        return Err(invalid());
    }
    Ok(Some(token.to_owned()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_path_is_per_port_under_a_quat_3d_directory() {
        // Every platform the engine runs on has a home directory in CI and
        // development; the exact base depends on the environment.
        let Some(path) = default_path(7878) else {
            return;
        };
        assert!(path.is_absolute(), "{}", path.display());
        assert!(path.ends_with(Path::new("quat-3d").join("remote-7878.token")));
        assert_ne!(default_path(7879), Some(path));
    }

    #[tokio::test]
    async fn reads_the_token_line() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("remote-7878.token");
        std::fs::write(&path, "0123abcd\n").unwrap();
        assert_eq!(read(&path).await.unwrap().as_deref(), Some("0123abcd"));
        std::fs::write(&path, "tok\r\n").unwrap();
        assert_eq!(read(&path).await.unwrap().as_deref(), Some("tok"));
    }

    #[tokio::test]
    async fn missing_file_is_no_token() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(read(&dir.path().join("absent")).await.unwrap(), None);
    }

    #[tokio::test]
    async fn rejects_empty_and_malformed_files() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("t");
        std::fs::write(&path, "\n").unwrap();
        assert!(matches!(
            read(&path).await,
            Err(TokenFileError::Empty { .. })
        ));
        for bad in [
            &b"two\nlines\n"[..],
            b"has space\n",
            b"\xff\xfe",
            &[b'a'; 5000][..],
        ] {
            std::fs::write(&path, bad).unwrap();
            assert!(
                matches!(read(&path).await, Err(TokenFileError::Invalid { .. })),
                "{bad:?}"
            );
        }
    }

    #[tokio::test]
    async fn unreadable_path_is_an_io_error() {
        let dir = tempfile::tempdir().unwrap();
        // A directory where the file should be.
        let err = read(dir.path()).await.unwrap_err();
        assert!(matches!(err, TokenFileError::Io { .. }), "{err:?}");
        assert!(err.to_string().contains(&dir.path().display().to_string()));
    }
}
