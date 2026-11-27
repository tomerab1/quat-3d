//! The HTTP layer the asset tools download through: a trait (so tests run
//! against an in-memory fake, never the network) and its reqwest client.

use std::future::Future;
use std::time::Duration;

use reqwest::Url;
use thiserror::Error;

/// Hosts downloads may come from besides the configured API's own.
pub const DOWNLOAD_HOSTS: &[&str] = &["dl.polyhaven.org"];
/// Most redirects followed per request.
const MAX_REDIRECTS: usize = 3;

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
    /// The URL (from an API response, or a redirect) is not one the asset
    /// tools may fetch.
    #[error("refusing to fetch {url}: {reason}")]
    Refused { url: String, reason: String },
}

/// Which URLs the asset tools may request. API responses are untrusted, so a
/// URL they name must be https on an allowed host ([`DOWNLOAD_HOSTS`] plus
/// the API's host), or share the configured API's exact origin (scheme, host
/// and port: a local mirror or test server). Anything else (plain http
/// elsewhere, loopback services such as the engine's own endpoint) is refused.
#[derive(Debug, Clone)]
pub struct UrlPolicy {
    api: Option<Url>,
    hosts: Vec<String>,
}

impl UrlPolicy {
    /// The policy for an API at `api` (a base URL).
    #[must_use]
    pub fn new(api: &str) -> Self {
        let api = Url::parse(api).ok();
        let mut hosts: Vec<String> = DOWNLOAD_HOSTS.iter().map(|h| (*h).to_owned()).collect();
        if let Some(host) = api.as_ref().and_then(Url::host_str) {
            hosts.push(host.to_ascii_lowercase());
        }
        Self { api, hosts }
    }

    /// Whether `url` may be requested.
    #[must_use]
    pub fn allows(&self, url: &Url) -> bool {
        if let Some(api) = &self.api
            && url.scheme() == api.scheme()
            && url.host_str() == api.host_str()
            && url.port_or_known_default() == api.port_or_known_default()
        {
            return true;
        }
        url.scheme() == "https"
            && url.username().is_empty()
            && url
                .host_str()
                .is_some_and(|h| self.hosts.iter().any(|a| a.eq_ignore_ascii_case(h)))
    }

    /// Parses `url` and checks it.
    ///
    /// # Errors
    /// [`HttpError::Refused`] for an unparsable or disallowed URL.
    pub fn check(&self, url: &str) -> Result<Url, HttpError> {
        let refused = |reason: &str| HttpError::Refused {
            url: url.to_owned(),
            reason: reason.to_owned(),
        };
        let parsed = Url::parse(url).map_err(|_| refused("not a valid URL"))?;
        if self.allows(&parsed) {
            Ok(parsed)
        } else {
            Err(refused(&format!(
                "only https from {} (or the configured API's origin) is allowed",
                self.hosts.join(", ")
            )))
        }
    }
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
/// Every request and every redirect hop must pass `policy`.
#[derive(Debug, Clone)]
pub struct ReqwestHttp {
    client: reqwest::Client,
    policy: UrlPolicy,
}

impl ReqwestHttp {
    /// # Errors
    /// The TLS backend cannot be initialised.
    pub fn new(timeout: Duration, policy: UrlPolicy) -> Result<Self, reqwest::Error> {
        let redirects = policy.clone();
        let client = reqwest::Client::builder()
            .user_agent(concat!("quat-mcp/", env!("CARGO_PKG_VERSION")))
            .timeout(timeout)
            .connect_timeout(Duration::from_secs(15))
            .redirect(reqwest::redirect::Policy::custom(move |attempt| {
                if attempt.previous().len() >= MAX_REDIRECTS {
                    attempt.error(format!("more than {MAX_REDIRECTS} redirects"))
                } else if redirects.allows(attempt.url()) {
                    attempt.follow()
                } else {
                    let to = attempt.url().to_string();
                    attempt.error(format!("redirect to {to} is not an allowed download host"))
                }
            }))
            .build()?;
        Ok(Self { client, policy })
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
        let checked = self.policy.check(url)?;
        let mut response = self
            .client
            .get(checked)
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
