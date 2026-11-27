//! Downloading a Poly Haven model into the project: the glTF and the files it
//! includes, confined to `assets/meshes/polyhaven/<id>/`, size-capped and
//! verified, with a licence / provenance record, optionally cooked.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime};

use md5::{Digest, Md5};
use serde::Serialize;
use serde_json::Value;

use super::http::{Http, UrlPolicy};
use super::paths::{POLYHAVEN_DIR, safe_relative, slash_path, url_file_name, valid_asset_id};
use super::polyhaven::{AssetError, FileEntry, PolyHaven};
use crate::content;

/// At most this many files per model (glTF + includes).
const MAX_FILES: usize = 256;
/// How long `quat-cook` may run.
const COOK_TIMEOUT: Duration = Duration::from_secs(600);
const LICENSE: &str = "CC0-1.0";
const LICENSE_URL: &str = "https://creativecommons.org/publicdomain/zero/1.0/";

/// What to fetch and how.
#[derive(Debug, Clone)]
pub struct FetchOptions {
    /// "1k", "2k", "4k", ... as Poly Haven names them.
    pub resolution: String,
    /// Cap on the total download.
    pub max_bytes: u64,
    /// Replace an earlier download of the same model.
    pub overwrite: bool,
    /// Run `quat-cook` (if it is on PATH) to make a GLB next to the glTF.
    pub cook: bool,
}

/// The fetch tool's result.
#[derive(Debug, Clone, Serialize)]
pub struct FetchReport {
    pub id: String,
    pub name: String,
    pub resolution: String,
    /// Project-relative path to spawn (`asset_spawn_model`): the cooked GLB
    /// when cooking succeeded, else the glTF.
    pub path: String,
    pub gltf: String,
    pub files: usize,
    pub bytes: u64,
    pub license: &'static str,
    pub source_url: String,
    pub authors: Vec<String>,
    /// Material count per glTF alphaMode (OPAQUE / MASK / BLEND).
    pub alpha_modes: BTreeMap<String, u32>,
    /// The model was already in the project (nothing downloaded).
    pub already_present: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cooked: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cook_note: Option<String>,
}

#[derive(Serialize)]
struct ProvenanceFile<'a> {
    path: String,
    url: &'a str,
    bytes: u64,
    #[serde(skip_serializing_if = "Option::is_none")]
    md5: Option<&'a str>,
}

#[derive(Serialize)]
struct Provenance<'a> {
    source: &'static str,
    id: &'a str,
    name: &'a str,
    source_url: &'a str,
    license: &'static str,
    license_url: &'static str,
    authors: &'a BTreeMap<String, String>,
    resolution: &'a str,
    fetched_at: String,
    files: Vec<ProvenanceFile<'a>>,
}

fn io_err(path: &Path) -> impl FnOnce(std::io::Error) -> AssetError + '_ {
    move |source| AssetError::Io {
        path: path.display().to_string(),
        source,
    }
}

/// The files to download: the glTF (named as in its URL) and its includes,
/// each with a validated relative path and a URL `policy` allows — all
/// checked before anything is requested.
fn plan<'a>(
    id: &str,
    resolution: &str,
    gltf: &'a FileEntry,
    max_bytes: u64,
    policy: &UrlPolicy,
) -> Result<Vec<(PathBuf, &'a FileEntry)>, AssetError> {
    let unsafe_path = |p: &str| AssetError::UnsafePath { path: p.to_owned() };
    let name = url_file_name(&gltf.url).ok_or_else(|| unsafe_path(&gltf.url))?;
    let gltf_path = safe_relative(name)
        .filter(|p| {
            p.components().count() == 1
                && p.extension()
                    .is_some_and(|e| e.eq_ignore_ascii_case("gltf"))
        })
        .ok_or_else(|| unsafe_path(name))?;
    let mut files = vec![(gltf_path, gltf)];
    for (rel, entry) in &gltf.include {
        files.push((safe_relative(rel).ok_or_else(|| unsafe_path(rel))?, entry));
    }
    for (_, entry) in &files {
        policy.check(&entry.url)?;
    }
    let total: u64 = files.iter().map(|(_, f)| f.size).sum();
    if files.len() > MAX_FILES || total > max_bytes {
        return Err(AssetError::TooLarge {
            id: id.to_owned(),
            resolution: resolution.to_owned(),
            bytes: total,
            limit: max_bytes,
        });
    }
    Ok(files)
}

/// Every external `buffers[].uri` / `images[].uri` of the glTF (percent-
/// decoded) must name one of the files downloaded with it, so the engine's
/// loader can only open files inside the model's folder; `data:` URIs are
/// embedded and fine.
fn check_gltf_uris(gltf: &[u8], planned: &[(PathBuf, &FileEntry)]) -> Result<(), AssetError> {
    let doc: Value = serde_json::from_slice(gltf).map_err(|e| AssetError::Corrupt {
        file: format!("glTF ({e})"),
        what: "JSON format",
    })?;
    let uris = ["buffers", "images"]
        .iter()
        .flat_map(|key| doc[*key].as_array().into_iter().flatten())
        .filter_map(|item| item.get("uri"));
    for uri in uris {
        let unsafe_uri = || AssetError::UnsafePath {
            path: uri.to_string(),
        };
        let Some(text) = uri.as_str() else {
            return Err(unsafe_uri());
        };
        if text.starts_with("data:") {
            continue;
        }
        let decoded = percent_encoding::percent_decode_str(text)
            .decode_utf8()
            .map_err(|_| unsafe_uri())?;
        let rel = safe_relative(&decoded).ok_or_else(unsafe_uri)?;
        if !planned.iter().skip(1).any(|(p, _)| *p == rel) {
            return Err(unsafe_uri());
        }
    }
    Ok(())
}

/// Material count per alphaMode in a glTF document.
fn alpha_modes(gltf: &[u8]) -> BTreeMap<String, u32> {
    let mut modes = BTreeMap::new();
    let doc: Value = serde_json::from_slice(gltf).unwrap_or(Value::Null);
    for m in doc["materials"].as_array().into_iter().flatten() {
        let mode = m["alphaMode"].as_str().unwrap_or("OPAQUE").to_owned();
        *modes.entry(mode).or_insert(0) += 1;
    }
    modes
}

/// Downloads `id` at `opts.resolution` under `<root>/assets/meshes/polyhaven/<id>/`.
///
/// # Errors
/// Invalid id, network / API errors, unsafe paths in the response, size cap,
/// corrupt downloads, filesystem errors.
pub async fn fetch<H: Http>(
    ph: &PolyHaven<H>,
    root: &Path,
    id: &str,
    opts: &FetchOptions,
) -> Result<FetchReport, AssetError> {
    if !valid_asset_id(id) {
        return Err(AssetError::InvalidId(id.to_owned()));
    }
    if !root.join("assets").is_dir() {
        return Err(AssetError::NoAssetsDir(root.display().to_string()));
    }
    let info = ph.info(id).await?;
    if info.kind.is_some_and(|k| k != 2) {
        return Err(AssetError::NotAModel(id.to_owned()));
    }
    let files = ph.files(id).await?;
    let Some(gltf) = files.gltf_at(&opts.resolution) else {
        return Err(AssetError::NoResolution {
            id: id.to_owned(),
            resolution: opts.resolution.clone(),
            available: files.gltf.keys().cloned().collect::<Vec<_>>().join(", "),
        });
    };
    let planned = plan(id, &opts.resolution, gltf, opts.max_bytes, ph.policy())?;

    let rel_dir = Path::new(POLYHAVEN_DIR).join(id);
    let dest = root.join(&rel_dir);
    let gltf_rel = rel_dir.join(&planned[0].0);
    let source_url = format!("https://polyhaven.com/a/{id}");
    let mut report = FetchReport {
        id: id.to_owned(),
        name: info.name.clone(),
        resolution: opts.resolution.clone(),
        path: slash_path(&gltf_rel),
        gltf: slash_path(&gltf_rel),
        files: planned.len(),
        bytes: planned.iter().map(|(_, f)| f.size).sum(),
        license: LICENSE,
        source_url: source_url.clone(),
        authors: info.authors.keys().cloned().collect(),
        alpha_modes: BTreeMap::new(),
        already_present: false,
        cooked: None,
        cook_note: None,
    };

    if dest.join(&planned[0].0).is_file() && !opts.overwrite {
        report.already_present = true;
        if let Ok(bytes) = tokio::fs::read(root.join(&gltf_rel)).await {
            report.alpha_modes = alpha_modes(&bytes);
        }
        return Ok(report);
    }

    // Download everything into a staging directory of this request's own
    // (concurrent fetches of the same id never share one), then move it in
    // place. Dropping the TempDir on an error removes it.
    let parent = root.join(POLYHAVEN_DIR);
    tokio::fs::create_dir_all(&parent)
        .await
        .map_err(io_err(&parent))?;
    let staging = tempfile::Builder::new()
        .prefix(&format!(".{id}."))
        .suffix(".partial")
        .tempdir_in(&parent)
        .map_err(io_err(&parent))?;
    report.alpha_modes = download(ph, staging.path(), &planned, &info.authors, &report).await?;
    let staged = staging.keep();
    if let Err(e) = replace_dir(&staged, &dest).await {
        let _ = tokio::fs::remove_dir_all(&staged).await;
        return Err(e);
    }

    if opts.cook {
        cook(root, &gltf_rel, id, &mut report).await;
    }
    Ok(report)
}

/// Downloads and verifies every planned file into `staging`, then writes the
/// provenance record and licence. Returns the glTF's alpha modes.
async fn download<H: Http>(
    ph: &PolyHaven<H>,
    staging: &Path,
    planned: &[(PathBuf, &FileEntry)],
    authors: &BTreeMap<String, String>,
    report: &FetchReport,
) -> Result<BTreeMap<String, u32>, AssetError> {
    let mut modes = BTreeMap::new();
    for (i, (rel, entry)) in planned.iter().enumerate() {
        let bytes = ph.http().get(&entry.url, entry.size).await?;
        let file = slash_path(rel);
        if bytes.len() as u64 != entry.size {
            return Err(AssetError::Corrupt { file, what: "size" });
        }
        if let Some(want) = &entry.md5
            && !format!("{:x}", Md5::digest(&bytes)).eq_ignore_ascii_case(want)
        {
            return Err(AssetError::Corrupt {
                file,
                what: "MD5 checksum",
            });
        }
        if i == 0 {
            // Before fetching anything it references.
            check_gltf_uris(&bytes, planned)?;
            modes = alpha_modes(&bytes);
        }
        let path = staging.join(rel);
        if let Some(parent) = path.parent() {
            tokio::fs::create_dir_all(parent)
                .await
                .map_err(io_err(parent))?;
        }
        tokio::fs::write(&path, &bytes)
            .await
            .map_err(io_err(&path))?;
    }

    let provenance = Provenance {
        source: "Poly Haven",
        id: &report.id,
        name: &report.name,
        source_url: &report.source_url,
        license: LICENSE,
        license_url: LICENSE_URL,
        authors,
        resolution: &report.resolution,
        fetched_at: humantime::format_rfc3339_seconds(SystemTime::now()).to_string(),
        files: planned
            .iter()
            .map(|(rel, f)| ProvenanceFile {
                path: slash_path(rel),
                url: &f.url,
                bytes: f.size,
                md5: f.md5.as_deref(),
            })
            .collect(),
    };
    let json = serde_json::to_vec_pretty(&provenance).unwrap_or_default();
    let path = staging.join("polyhaven.json");
    tokio::fs::write(&path, json).await.map_err(io_err(&path))?;

    let license =
        content::render_document("asset_license", &provenance).map_err(|e| AssetError::Io {
            path: "LICENSE.md".to_owned(),
            source: std::io::Error::other(e.to_string()),
        })?;
    let path = staging.join("LICENSE.md");
    tokio::fs::write(&path, license)
        .await
        .map_err(io_err(&path))?;
    Ok(modes)
}

async fn replace_dir(staging: &Path, dest: &Path) -> Result<(), AssetError> {
    if dest.exists() {
        tokio::fs::remove_dir_all(dest)
            .await
            .map_err(io_err(dest))?;
    }
    tokio::fs::rename(staging, dest).await.map_err(io_err(dest))
}

/// Runs `quat-cook <model>.gltf <id>.glb` when the cooker is on PATH; the
/// outcome goes into the report (cooking is optional, never an error).
async fn cook(root: &Path, gltf_rel: &Path, id: &str, report: &mut FetchReport) {
    let Ok(cooker) = which::which("quat-cook") else {
        report.cook_note = Some("quat-cook is not on PATH; the glTF is used as is".to_owned());
        return;
    };
    let glb_rel = gltf_rel.with_file_name(format!("{id}.glb"));
    let run = tokio::process::Command::new(cooker)
        .arg(root.join(gltf_rel))
        .arg(root.join(&glb_rel))
        .kill_on_drop(true)
        .output();
    match tokio::time::timeout(COOK_TIMEOUT, run).await {
        Ok(Ok(out)) if out.status.success() => {
            report.cooked = Some(slash_path(&glb_rel));
            report.path = slash_path(&glb_rel);
        }
        Ok(Ok(out)) => {
            let stderr = String::from_utf8_lossy(&out.stderr);
            report.cook_note = Some(format!(
                "quat-cook failed ({}); the glTF is used as is: {}",
                out.status,
                stderr.trim().chars().take(400).collect::<String>()
            ));
        }
        Ok(Err(e)) => report.cook_note = Some(format!("quat-cook did not start: {e}")),
        Err(_) => report.cook_note = Some("quat-cook timed out; the glTF is used as is".to_owned()),
    }
}
