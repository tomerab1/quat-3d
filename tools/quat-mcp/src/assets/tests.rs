//! Asset search / fetch against an in-memory HTTP fake: no network.

use std::collections::HashMap;
use std::path::Path;
use std::sync::Mutex;

use md5::{Digest, Md5};
use serde_json::{Value, json};

use super::*;

const API: &str = "https://api.test";

/// URL → body; anything else is a 404. Records every request.
#[derive(Default)]
struct FakeHttp {
    bodies: HashMap<String, Vec<u8>>,
    requests: Mutex<Vec<String>>,
    offline: bool,
}

impl FakeHttp {
    fn with(mut self, url: &str, body: impl Into<Vec<u8>>) -> Self {
        self.bodies.insert(url.to_owned(), body.into());
        self
    }
    fn json(self, url: &str, v: &Value) -> Self {
        self.with(url, v.to_string())
    }
    fn requested(&self, url: &str) -> bool {
        self.requests.lock().unwrap().iter().any(|r| r == url)
    }
}

impl Http for FakeHttp {
    async fn get(&self, url: &str, max_bytes: u64) -> Result<Vec<u8>, HttpError> {
        self.requests.lock().unwrap().push(url.to_owned());
        if self.offline {
            return Err(HttpError::Unreachable {
                url: url.to_owned(),
                reason: "connection refused".to_owned(),
            });
        }
        let body = self.bodies.get(url).ok_or_else(|| HttpError::Status {
            url: url.to_owned(),
            status: 404,
        })?;
        if body.len() as u64 > max_bytes {
            return Err(HttpError::TooLarge {
                url: url.to_owned(),
                limit: max_bytes,
            });
        }
        Ok(body.clone())
    }
}

fn md5_hex(b: &[u8]) -> String {
    format!("{:x}", Md5::digest(b))
}

const GLTF: &str = r#"{"asset":{"version":"2.0"},"materials":[{"name":"bark"},{"name":"leaves","alphaMode":"MASK"},{"alphaMode":"MASK"}],"buffers":[{"uri":"fir_sapling.bin","byteLength":10}],"images":[{"uri":"textures/fir%5Fbark_diff_1k.jpg"},{"uri":"data:image/png;base64,AAAA"}]}"#;
const BIN: &[u8] = b"\x00\x01\x02\x03binary";
const TEX: &[u8] = b"jpeg bytes";

fn entry(url: &str, body: &[u8]) -> Value {
    json!({"url": url, "size": body.len(), "md5": md5_hex(body)})
}

/// A fake Poly Haven with one model, `fir_sapling`, whose 1k glTF includes
/// `include` (path → body) — by default a .bin and a texture.
fn fake(include: &[(&str, &[u8])]) -> FakeHttp {
    fake_with(GLTF, "https://dl.polyhaven.org/", include)
}

/// `fake` with a given glTF document, served (with its includes) from
/// `base`.
fn fake_with(gltf_doc: &str, base: &str, include: &[(&str, &[u8])]) -> FakeHttp {
    let gltf_url = format!("{base}fir_sapling_1k.gltf");
    let gltf_url = gltf_url.as_str();
    let mut inc = serde_json::Map::new();
    let mut http = FakeHttp::default().with(gltf_url, gltf_doc);
    for (i, (path, body)) in include.iter().enumerate() {
        let url = format!("{base}f{i}");
        inc.insert((*path).to_owned(), entry(&url, body));
        http = http.with(&url, *body);
    }
    let mut gltf = entry(gltf_url, gltf_doc.as_bytes());
    gltf["include"] = Value::Object(inc);
    http.json(
        &format!("{API}/info/fir_sapling"),
        &json!({"name": "Fir Sapling", "type": 2, "authors": {"Jane Doe": "All"},
                "categories": ["nature", "plants"], "tags": ["tree", "conifer"],
                "download_count": 900}),
    )
    .json(
        &format!("{API}/files/fir_sapling"),
        &json!({"gltf": {"1k": {"gltf": gltf}}, "blend": {}}),
    )
}

fn default_fake() -> FakeHttp {
    fake(&[
        ("fir_sapling.bin", BIN),
        ("textures/fir_bark_diff_1k.jpg", TEX),
    ])
}

fn project() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::create_dir_all(dir.path().join("assets")).unwrap();
    dir
}

fn opts() -> FetchOptions {
    FetchOptions {
        resolution: "1k".to_owned(),
        max_bytes: 1 << 20,
        overwrite: false,
        cook: false,
    }
}

fn model_dir(root: &Path) -> std::path::PathBuf {
    root.join("assets/meshes/polyhaven/fir_sapling")
}

#[tokio::test]
async fn fetch_writes_the_model_licence_and_provenance() {
    let root = project();
    let ph = PolyHaven::new(default_fake(), API);
    let r = fetch(&ph, root.path(), "fir_sapling", &opts())
        .await
        .unwrap();
    assert_eq!(
        r.path,
        "assets/meshes/polyhaven/fir_sapling/fir_sapling_1k.gltf"
    );
    assert_eq!(r.files, 3);
    assert_eq!(r.license, "CC0-1.0");
    assert_eq!(r.authors, vec!["Jane Doe".to_owned()]);
    assert_eq!(r.alpha_modes.get("MASK"), Some(&2));
    assert_eq!(r.alpha_modes.get("OPAQUE"), Some(&1));
    assert!(!r.already_present);

    let dir = model_dir(root.path());
    assert_eq!(std::fs::read(dir.join("fir_sapling.bin")).unwrap(), BIN);
    assert_eq!(
        std::fs::read(dir.join("textures/fir_bark_diff_1k.jpg")).unwrap(),
        TEX
    );
    let license = std::fs::read_to_string(dir.join("LICENSE.md")).unwrap();
    assert!(
        license.contains("CC0-1.0") && license.contains("Jane Doe"),
        "{license}"
    );
    assert!(
        license.contains("https://polyhaven.com/a/fir_sapling"),
        "{license}"
    );
    let prov: Value =
        serde_json::from_slice(&std::fs::read(dir.join("polyhaven.json")).unwrap()).unwrap();
    assert_eq!(prov["license"], "CC0-1.0");
    assert_eq!(prov["source_url"], "https://polyhaven.com/a/fir_sapling");
    assert_eq!(prov["authors"]["Jane Doe"], "All");
    assert_eq!(prov["files"].as_array().unwrap().len(), 3);
    // No staging directory is left behind.
    let leftovers: Vec<_> = std::fs::read_dir(root.path().join("assets/meshes/polyhaven"))
        .unwrap()
        .filter_map(Result::ok)
        .map(|e| e.file_name())
        .collect();
    assert_eq!(leftovers, vec![std::ffi::OsString::from("fir_sapling")]);

    // Fetching again downloads nothing.
    let ph2 = PolyHaven::new(default_fake(), API);
    let again = fetch(&ph2, root.path(), "fir_sapling", &opts())
        .await
        .unwrap();
    assert!(again.already_present);
    assert!(!ph2.http().requested("https://dl.polyhaven.org/f0"));
}

#[tokio::test]
async fn include_paths_cannot_escape_the_model_directory() {
    for bad in [
        "../../evil.bin",
        "/etc/evil.bin",
        "textures/../../../evil.bin",
        ".hidden",
    ] {
        let root = project();
        let ph = PolyHaven::new(fake(&[(bad, BIN)]), API);
        let err = fetch(&ph, root.path(), "fir_sapling", &opts())
            .await
            .unwrap_err();
        assert!(matches!(err, AssetError::UnsafePath { .. }), "{bad}: {err}");
        assert!(
            !ph.http().requested("https://dl.polyhaven.org/f0"),
            "nothing downloaded"
        );
        assert!(
            !root.path().join("assets/meshes").exists(),
            "nothing written"
        );
        assert!(!root.path().join("evil.bin").exists());
    }
}

#[tokio::test]
async fn download_urls_must_be_https_on_an_allowed_host() {
    for base in [
        "http://dl.polyhaven.org/",       // plain http
        "https://127.0.0.1:7878/",        // a loopback service (the engine's endpoint)
        "https://evil.example/",          // another host
        "https://user@dl.polyhaven.org/", // credentials
        "ftp://dl.polyhaven.org/",
    ] {
        let root = project();
        let ph = PolyHaven::new(fake_with(GLTF, base, &[("fir_sapling.bin", BIN)]), API);
        let err = fetch(&ph, root.path(), "fir_sapling", &opts())
            .await
            .unwrap_err();
        assert!(
            matches!(err, AssetError::Http(HttpError::Refused { .. })),
            "{base}: {err}"
        );
        let requests = ph.http().requests.lock().unwrap().clone();
        assert!(
            requests.iter().all(|r| r.starts_with(API)),
            "{base}: nothing but the API was requested: {requests:?}"
        );
        assert!(
            !root
                .path()
                .join("assets/meshes/polyhaven/fir_sapling")
                .exists()
        );
    }
    // The configured API's own origin is allowed (a local mirror).
    let root = project();
    let ph = PolyHaven::new(
        fake_with(
            GLTF,
            &format!("{API}/dl/"),
            &[
                ("fir_sapling.bin", BIN),
                ("textures/fir_bark_diff_1k.jpg", TEX),
            ],
        ),
        API,
    );
    fetch(&ph, root.path(), "fir_sapling", &opts())
        .await
        .unwrap();
}

#[tokio::test]
async fn gltf_uris_must_name_the_downloaded_files() {
    for uri in [
        "../../../secret.bin",
        "/etc/passwd",
        "%2E%2E/%2E%2E/secret.bin",
        "other.bin",
        "file:///etc/passwd",
    ] {
        let doc = json!({"asset": {"version": "2.0"}, "buffers": [{"uri": uri}]}).to_string();
        let root = project();
        let ph = PolyHaven::new(
            fake_with(
                &doc,
                "https://dl.polyhaven.org/",
                &[("fir_sapling.bin", BIN)],
            ),
            API,
        );
        let err = fetch(&ph, root.path(), "fir_sapling", &opts())
            .await
            .unwrap_err();
        assert!(matches!(err, AssetError::UnsafePath { .. }), "{uri}: {err}");
        assert!(
            !ph.http().requested("https://dl.polyhaven.org/f0"),
            "{uri}: includes are not fetched"
        );
        let polyhaven = root.path().join("assets/meshes/polyhaven");
        assert_eq!(
            std::fs::read_dir(&polyhaven).map_or(0, Iterator::count),
            0,
            "{uri}"
        );
    }
}

#[tokio::test]
async fn concurrent_fetches_of_one_model_do_not_collide() {
    let root = project();
    let ph = PolyHaven::new(default_fake(), API);
    let again = FetchOptions {
        overwrite: true,
        ..opts()
    };
    let (a, b) = tokio::join!(
        fetch(&ph, root.path(), "fir_sapling", &again),
        fetch(&ph, root.path(), "fir_sapling", &again)
    );
    // Each request stages privately; both may finish, or the second rename
    // may lose to the first, but the model ends up complete either way.
    assert!(a.is_ok() || b.is_ok(), "{a:?} {b:?}");
    let dir = model_dir(root.path());
    assert_eq!(std::fs::read(dir.join("fir_sapling.bin")).unwrap(), BIN);
    assert_eq!(
        std::fs::read(dir.join("textures/fir_bark_diff_1k.jpg")).unwrap(),
        TEX
    );
    assert!(dir.join("LICENSE.md").is_file());
    let leftovers: Vec<_> = std::fs::read_dir(root.path().join("assets/meshes/polyhaven"))
        .unwrap()
        .filter_map(Result::ok)
        .map(|e| e.file_name())
        .collect();
    assert_eq!(leftovers, vec![std::ffi::OsString::from("fir_sapling")]);
}

#[tokio::test]
async fn the_size_cap_applies_before_downloading() {
    let root = project();
    let ph = PolyHaven::new(default_fake(), API);
    let small = FetchOptions {
        max_bytes: 50,
        ..opts()
    };
    let err = fetch(&ph, root.path(), "fir_sapling", &small)
        .await
        .unwrap_err();
    assert!(matches!(err, AssetError::TooLarge { .. }), "{err}");
    assert!(err.to_string().contains("lower resolution"), "{err}");
    assert!(
        !ph.http()
            .requested("https://dl.polyhaven.org/fir_sapling_1k.gltf")
    );
}

#[tokio::test]
async fn corrupt_downloads_leave_nothing_behind() {
    let root = project();
    // The API promises the texture's checksum; the server sends other bytes.
    let mut http = default_fake();
    http.bodies.insert(
        "https://dl.polyhaven.org/f1".to_owned(),
        b"jpeg byteZ".to_vec(),
    );
    let ph = PolyHaven::new(http, API);
    let err = fetch(&ph, root.path(), "fir_sapling", &opts())
        .await
        .unwrap_err();
    assert!(
        matches!(
            err,
            AssetError::Corrupt {
                what: "MD5 checksum",
                ..
            }
        ),
        "{err}"
    );
    let polyhaven = root.path().join("assets/meshes/polyhaven");
    assert_eq!(std::fs::read_dir(&polyhaven).map_or(0, Iterator::count), 0);
}

#[tokio::test]
async fn bad_ids_resolutions_and_projects_are_reported() {
    let root = project();
    let ph = PolyHaven::new(default_fake(), API);
    let err = fetch(&ph, root.path(), "../etc", &opts())
        .await
        .unwrap_err();
    assert!(matches!(err, AssetError::InvalidId(_)), "{err}");
    assert!(ph.http().requests.lock().unwrap().is_empty());

    let err = fetch(&ph, root.path(), "oak_tree", &opts())
        .await
        .unwrap_err();
    assert!(matches!(err, AssetError::NotFound(_)), "{err}");

    let two_k = FetchOptions {
        resolution: "2k".to_owned(),
        ..opts()
    };
    let err = fetch(&ph, root.path(), "fir_sapling", &two_k)
        .await
        .unwrap_err();
    assert!(err.to_string().contains("available: 1k"), "{err}");

    let empty = tempfile::tempdir().unwrap();
    let err = fetch(&ph, empty.path(), "fir_sapling", &opts())
        .await
        .unwrap_err();
    assert!(matches!(err, AssetError::NoAssetsDir(_)), "{err}");

    let offline = PolyHaven::new(
        FakeHttp {
            offline: true,
            ..FakeHttp::default()
        },
        API,
    );
    let err = fetch(&offline, root.path(), "fir_sapling", &opts())
        .await
        .unwrap_err();
    assert!(err.to_string().contains("could not reach"), "{err}");
}

#[tokio::test]
async fn search_matches_words_sorts_and_reports_sizes() {
    let assets = json!({
        "fir_sapling": {"name": "Fir Sapling", "type": 2, "tags": ["tree", "conifer"],
                        "categories": ["nature"], "download_count": 900,
                        "authors": {"Jane Doe": "All"}, "polycount": 12000},
        "pine_tree_01": {"name": "Pine Tree 01", "type": 2, "tags": ["conifer"],
                         "categories": ["nature"], "download_count": 5000},
        "office_chair": {"name": "Office Chair", "type": 2, "tags": ["furniture"],
                         "categories": ["furniture"], "download_count": 99999}
    });
    let http = default_fake()
        .json(&format!("{API}/assets?t=models"), &assets)
        .json(
            &format!("{API}/categories/models"),
            &json!({"nature": 2, "furniture": 1}),
        );
    let ph = PolyHaven::new(http, API);

    let r = ph.search("CONIFER", None, 10).await.unwrap();
    assert_eq!(r.matches, 2);
    let ids: Vec<_> = r.hits.iter().map(|h| h.id.as_str()).collect();
    assert_eq!(
        ids,
        ["pine_tree_01", "fir_sapling"],
        "most downloaded first"
    );
    let fir = &r.hits[1];
    assert_eq!(fir.polycount, Some(12000));
    assert_eq!(fir.authors, vec!["Jane Doe".to_owned()]);
    let bytes = (GLTF.len() + BIN.len() + TEX.len()) as u64;
    assert_eq!(fir.download_bytes.as_ref().unwrap()["1k"], bytes);
    assert!(
        r.hits[0].download_bytes.is_none(),
        "no /files for pine: null sizes"
    );

    // Every word must match; the limit cuts the list but not the count.
    assert_eq!(ph.search("fir tree", None, 10).await.unwrap().matches, 1);
    let one = ph.search("", None, 1).await.unwrap();
    assert_eq!((one.matches, one.hits.len()), (3, 1));

    // Nothing found: the categories that exist.
    let none = ph.search("spaceship", None, 10).await.unwrap();
    assert_eq!(none.matches, 0);
    assert_eq!(none.categories.unwrap()["nature"], 2);
}

#[tokio::test]
async fn search_encodes_the_category() {
    let http = FakeHttp::default()
        .json(
            &format!("{API}/assets?t=models&c=food+%26+drink"),
            &json!({"apple": {"name": "Apple", "type": 2}}),
        )
        .json(&format!("{API}/files/apple"), &json!({}));
    let ph = PolyHaven::new(http, API);
    let r = ph.search("apple", Some("food & drink"), 5).await.unwrap();
    assert_eq!(r.matches, 1);
}
