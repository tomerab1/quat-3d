//! The HTTP layer the asset tools download through: a trait (so tests run
//! against an in-memory fake, never the network) and its reqwest client.

use std::future::Future;
use std::time::Duration;

use thiserror::Error;

/// Why a download failed.
#[derive(Debug, Error)]
pub enum HttpError {
    /// No connection: offline, DNS, refused, TLS, proxy, timeout.
    #[error("could not reach {url}: {reason}")]
    Unreachable { url: String, reason: String },
    /// The server answered with an error status.
    #[error("{url} answered HTTP {status}")]
    Status { url: String, status: u16 },
    /// The body is larger than the caller allows.
    #[error("{url} is larger than the {limit}-byte limit")]
    TooLarge { url: String, limit: u64 },
}

/// Fetches whole response bodies.
pub trait Http: Send + Sync {
    /// GET `url` and return its body, failing with [`HttpError::TooLarge`]
    /// past `max_bytes` (checked against Content-Length and while reading).
    fn get(
        &self,
        url: &str,
        max_bytes: u64,
    ) -> impl Future<Output = Result<Vec<u8>, HttpError>> + Send;
}

/// The real client: rustls with the platform's trust store, the standard
/// proxy environment variables, an identifying user agent and a timeout.
#[derive(Debug, Clone)]
pub struct ReqwestHttp {
    client: reqwest::Client,
}

impl ReqwestHttp {
    /// # Errors
    /// The TLS backend cannot be initialised.
    pub fn new(timeout: Duration) -> Result<Self, reqwest::Error> {
        let client = reqwest::Client::builder()
            .user_agent(concat!("quat-mcp/", env!("CARGO_PKG_VERSION")))
            .timeout(timeout)
            .connect_timeout(Duration::from_secs(15))
            .build()?;
        Ok(Self { client })
    }
}

fn unreachable(url: &str, e: &reqwest::Error) -> HttpError {
    // The chain carries the useful part (connection refused, DNS, proxy).
    let mut reason = e.to_string();
    let mut source = std::error::Error::source(e);
    while let Some(s) = source {
        reason.push_str(": ");
        reason.push_str(&s.to_string());
        source = s.source();
    }
    HttpError::Unreachable {
        url: url.to_owned(),
        reason,
    }
}

impl Http for ReqwestHttp {
    async fn get(&self, url: &str, max_bytes: u64) -> Result<Vec<u8>, HttpError> {
        let mut response = self
            .client
            .get(url)
            .send()
            .await
            .map_err(|e| unreachable(url, &e))?;
        let status = response.status();
        if !status.is_success() {
            return Err(HttpError::Status {
                url: url.to_owned(),
                status: status.as_u16(),
            });
        }
        let too_large = || HttpError::TooLarge {
            url: url.to_owned(),
            limit: max_bytes,
        };
        if response.content_length().is_some_and(|n| n > max_bytes) {
            return Err(too_large());
        }
        let mut body = Vec::new();
        while let Some(chunk) = response.chunk().await.map_err(|e| unreachable(url, &e))? {
            if (body.len() + chunk.len()) as u64 > max_bytes {
                return Err(too_large());
            }
            body.extend_from_slice(&chunk);
        }
        Ok(body)
    }
}
