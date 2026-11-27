//! The public Poly Haven API (<https://api.polyhaven.com>, CC0 assets):
//! response types and model search.
//!
//! - `GET /assets?t=models[&c=<category>]` → `{id: AssetInfo}`
//! - `GET /info/<id>` → `AssetInfo`
//! - `GET /files/<id>` → `{"gltf": {"1k": {"gltf": FileEntry}}, ...}`
//! - `GET /categories/models` → `{category: count}`

use std::collections::BTreeMap;

use futures::future::join_all;
use serde::{Deserialize, Serialize};
use thiserror::Error;

use super::http::{Http, HttpError};

/// Largest API (JSON) response accepted.
const MAX_JSON_BYTES: u64 = 32 * 1024 * 1024;
/// Poly Haven's asset type number for models.
const MODEL_TYPE: u32 = 2;

/// Why an asset tool failed.
#[derive(Debug, Error)]
pub enum AssetError {
    #[error(transparent)]
    Http(#[from] HttpError),
    #[error("unexpected Poly Haven API response from {url}: {reason}")]
    BadResponse { url: String, reason: String },
    #[error("`{0}` is not a valid Poly Haven id (lowercase letters, digits, `_` and `-`)")]
    InvalidId(String),
    #[error("Poly Haven has no asset `{0}`; find ids with asset_search")]
    NotFound(String),
    #[error("`{0}` is not a model (it is an HDRI or a texture); asset_search lists models")]
    NotAModel(String),
    #[error("`{id}` has no glTF at {resolution}; available: {available}")]
    NoResolution {
        id: String,
        resolution: String,
        available: String,
    },
    #[error("refusing `{path}` from the API response: not a safe relative path")]
    UnsafePath { path: String },
    #[error(
        "`{id}` at {resolution} is {bytes} bytes, over the {limit}-byte limit; pick a lower resolution"
    )]
    TooLarge {
        id: String,
        resolution: String,
        bytes: u64,
        limit: u64,
    },
    #[error("download of `{file}` does not match the API's {what}; try again")]
    Corrupt { file: String, what: &'static str },
    #[error("{path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error(
        "the project root {0} has no assets/ directory; start quat-mcp in the quat-3d \
         checkout or pass --project-root"
    )]
    NoAssetsDir(String),
}

/// One asset as `/assets` and `/info` describe it (fields we use).
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct AssetInfo {
    pub name: String,
    #[serde(rename = "type")]
    pub kind: Option<u32>,
    pub categories: Vec<String>,
    pub tags: Vec<String>,
    /// Author name → credited role.
    pub authors: BTreeMap<String, String>,
    pub polycount: Option<u64>,
    pub download_count: u64,
}

/// A downloadable file in `/files`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct FileEntry {
    pub url: String,
    pub size: u64,
    pub md5: Option<String>,
    /// Files the glTF references, keyed by their path relative to it.
    pub include: BTreeMap<String, FileEntry>,
}

/// The glTF format of `/files`: resolution → `{"gltf": FileEntry}`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(default)]
pub struct Files {
    pub gltf: BTreeMap<String, BTreeMap<String, FileEntry>>,
}

impl Files {
    /// The glTF entry at `resolution`, if offered.
    #[must_use]
    pub fn gltf_at(&self, resolution: &str) -> Option<&FileEntry> {
        self.gltf.get(resolution)?.get("gltf")
    }

    /// Download size (glTF + includes) per resolution.
    #[must_use]
    pub fn sizes(&self) -> BTreeMap<String, u64> {
        self.gltf
            .iter()
            .filter_map(|(res, formats)| {
                let f = formats.get("gltf")?;
                Some((
                    res.clone(),
                    f.size + f.include.values().map(|i| i.size).sum::<u64>(),
                ))
            })
            .collect()
    }
}

/// One search hit as the tool returns it.
#[derive(Debug, Clone, Serialize)]
pub struct SearchHit {
    pub id: String,
    pub name: String,
    pub categories: Vec<String>,
    pub tags: Vec<String>,
    pub authors: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub polycount: Option<u64>,
    pub downloads: u64,
    /// glTF download size in bytes per resolution (null if the lookup failed).
    pub download_bytes: Option<BTreeMap<String, u64>>,
}

/// Search results: the best `hits` and how many assets matched in all.
#[derive(Debug, Clone, Serialize)]
pub struct SearchResult {
    pub matches: usize,
    pub hits: Vec<SearchHit>,
    /// When nothing matched: the categories that exist (with model counts).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub categories: Option<BTreeMap<String, u64>>,
}

/// A Poly Haven API client over some [`Http`].
#[derive(Debug, Clone)]
pub struct PolyHaven<H> {
    http: H,
    api: String,
}

impl<H: Http> PolyHaven<H> {
    /// `api` is the API base URL, e.g. `https://api.polyhaven.com`.
    pub fn new(http: H, api: impl Into<String>) -> Self {
        Self {
            http,
            api: api.into().trim_end_matches('/').to_owned(),
        }
    }

    pub(super) fn http(&self) -> &H {
        &self.http
    }

    async fn json<T: for<'de> Deserialize<'de>>(&self, path: &str) -> Result<T, AssetError> {
        self.json_query(path, &[]).await
    }

    /// GET `<api><path>?<query>` (query values URL-encoded) as JSON.
    async fn json_query<T: for<'de> Deserialize<'de>>(
        &self,
        path: &str,
        query: &[(&str, &str)],
    ) -> Result<T, AssetError> {
        let base = format!("{}{path}", self.api);
        let url = if query.is_empty() {
            base
        } else {
            reqwest::Url::parse_with_params(&base, query)
                .map_err(|e| AssetError::BadResponse {
                    url: base.clone(),
                    reason: format!("bad API URL: {e}"),
                })?
                .to_string()
        };
        let body = self.http.get(&url, MAX_JSON_BYTES).await?;
        serde_json::from_slice(&body).map_err(|e| AssetError::BadResponse {
            url,
            reason: e.to_string(),
        })
    }

    /// `/info/<id>`; [`AssetError::NotFound`] for an unknown id.
    ///
    /// # Errors
    /// Network, API or not-found errors.
    pub async fn info(&self, id: &str) -> Result<AssetInfo, AssetError> {
        match self.json::<AssetInfo>(&format!("/info/{id}")).await {
            Err(AssetError::Http(HttpError::Status { status: 404, .. })) => {
                Err(AssetError::NotFound(id.to_owned()))
            }
            Ok(info) if info.name.is_empty() => Err(AssetError::NotFound(id.to_owned())),
            other => other,
        }
    }

    /// `/files/<id>`.
    ///
    /// # Errors
    /// Network, API or not-found errors.
    pub async fn files(&self, id: &str) -> Result<Files, AssetError> {
        match self.json::<Files>(&format!("/files/{id}")).await {
            Err(AssetError::Http(HttpError::Status { status: 404, .. })) => {
                Err(AssetError::NotFound(id.to_owned()))
            }
            other => other,
        }
    }

    /// Models matching every word of `query` (in the id, name, tags or
    /// categories; case-insensitive), optionally within `category`, most
    /// downloaded first; at most `limit` hits, each with its download sizes.
    ///
    /// # Errors
    /// Network or API errors.
    pub async fn search(
        &self,
        query: &str,
        category: Option<&str>,
        limit: usize,
    ) -> Result<SearchResult, AssetError> {
        let mut query_params = vec![("t", "models")];
        if let Some(c) = category {
            query_params.push(("c", c));
        }
        let all: BTreeMap<String, AssetInfo> = self.json_query("/assets", &query_params).await?;
        let words: Vec<String> = query.split_whitespace().map(str::to_lowercase).collect();
        let mut matches: Vec<(String, AssetInfo)> = all
            .into_iter()
            .filter(|(id, info)| {
                info.kind.is_none_or(|k| k == MODEL_TYPE)
                    && words.iter().all(|w| matches_word(id, info, w))
            })
            .collect();
        if matches.is_empty() {
            return self.no_matches().await;
        }
        matches.sort_by(|a, b| {
            b.1.download_count
                .cmp(&a.1.download_count)
                .then(a.0.cmp(&b.0))
        });
        let total = matches.len();
        matches.truncate(limit);
        let sizes = join_all(matches.iter().map(|(id, _)| self.files(id))).await;
        let hits = matches
            .into_iter()
            .zip(sizes)
            .map(|((id, info), files)| SearchHit {
                id,
                name: info.name,
                categories: info.categories,
                tags: info.tags.into_iter().take(12).collect(),
                authors: info.authors.into_keys().collect(),
                polycount: info.polycount,
                downloads: info.download_count,
                download_bytes: files.ok().map(|f| f.sizes()),
            })
            .collect();
        Ok(SearchResult {
            matches: total,
            hits,
            categories: None,
        })
    }

    async fn no_matches(&self) -> Result<SearchResult, AssetError> {
        let categories: BTreeMap<String, u64> = self.json("/categories/models").await?;
        Ok(SearchResult {
            matches: 0,
            hits: Vec::new(),
            categories: Some(categories),
        })
    }
}

fn matches_word(id: &str, info: &AssetInfo, word: &str) -> bool {
    id.contains(word)
        || info.name.to_lowercase().contains(word)
        || info
            .tags
            .iter()
            .chain(&info.categories)
            .any(|t| t.to_lowercase().contains(word))
}
