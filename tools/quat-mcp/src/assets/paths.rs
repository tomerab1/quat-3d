//! Path checks for downloaded assets: everything an API response names is
//! untrusted, so ids and relative file paths are validated before anything
//! touches the filesystem.

use std::path::{Component, Path, PathBuf};

/// Where fetched Poly Haven models live, relative to the project root.
pub const POLYHAVEN_DIR: &str = "assets/meshes/polyhaven";

/// A Poly Haven asset id: 1..=64 of `[a-z0-9_-]` (their slugs).
#[must_use]
pub fn valid_asset_id(id: &str) -> bool {
    (1..=64).contains(&id.len())
        && id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_' || b == b'-')
}

/// A relative path from an API response (a glTF `include` key or file name)
/// made safe to join under the asset directory: only plain components of
/// `[A-Za-z0-9._-]`, at most 8 deep, no `.`/`..`, no absolute or drive
/// prefixes, no backslashes, no hidden files. `None` when unsafe.
#[must_use]
pub fn safe_relative(path: &str) -> Option<PathBuf> {
    if path.is_empty() || path.len() > 255 || path.contains('\\') {
        return None;
    }
    let mut out = PathBuf::new();
    let mut depth = 0;
    for component in Path::new(path).components() {
        let Component::Normal(part) = component else {
            return None;
        };
        let part = part.to_str()?;
        let allowed = |b: u8| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_' | b'-');
        if part.starts_with('.') || !part.bytes().all(allowed) {
            return None;
        }
        depth += 1;
        out.push(part);
    }
    (depth > 0 && depth <= 8).then_some(out)
}

/// The last path segment of a download URL (the glTF file's own name, which
/// its `include` files are relative to).
#[must_use]
pub fn url_file_name(url: &str) -> Option<&str> {
    let path = url.split(['?', '#']).next()?;
    let name = path.rsplit('/').next()?;
    (!name.is_empty()).then_some(name)
}

/// Forward-slash form of a project-relative path (what the engine's
/// `asset_spawn_model` takes on every platform).
#[must_use]
pub fn slash_path(path: &Path) -> String {
    path.components()
        .filter_map(|c| match c {
            Component::Normal(p) => p.to_str(),
            _ => None,
        })
        .collect::<Vec<_>>()
        .join("/")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids() {
        assert!(valid_asset_id("rock_moss_set_01"));
        assert!(valid_asset_id("fir-sapling"));
        for bad in ["", "Rock", "../x", "a/b", "a b", &"x".repeat(65)] {
            assert!(!valid_asset_id(bad), "{bad}");
        }
    }

    #[test]
    fn relative_paths_are_confined() {
        assert_eq!(
            safe_relative("textures/rock_diff_1k.jpg"),
            Some(PathBuf::from("textures").join("rock_diff_1k.jpg"))
        );
        assert_eq!(safe_relative("rock.bin"), Some(PathBuf::from("rock.bin")));
        for bad in [
            "",
            "../evil.bin",
            "textures/../../evil.bin",
            "/etc/passwd",
            "./x.bin",
            "a\\b.bin",
            "C:evil.bin",
            ".hidden",
            "textures/.git/config",
            "x y.png",
            "a/b/c/d/e/f/g/h/i.png",
        ] {
            assert_eq!(safe_relative(bad), None, "{bad}");
        }
    }

    #[test]
    fn url_names() {
        assert_eq!(
            url_file_name("https://dl.polyhaven.org/file/x/1k/rock/rock_1k.gltf?a=1"),
            Some("rock_1k.gltf")
        );
        assert_eq!(url_file_name("https://h/dir/"), None);
        assert_eq!(
            slash_path(Path::new("assets").join("meshes").join("a.gltf").as_path()),
            "assets/meshes/a.gltf"
        );
    }
}
