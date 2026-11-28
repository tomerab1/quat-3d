//! Inputs of the gameplay script tools (`script_*`, docs/remote-control.md
//! "Gameplay scripts"), and the client-side checks that answer obvious
//! mistakes without a round trip (the engine checks everything again).

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Largest script source the engine accepts (bytes).
pub const MAX_SCRIPT_BYTES: usize = 256 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScriptPathParams {
    /// Script path relative to `assets/scripts/`, ending in `.lua`, with `/`
    /// separators, e.g. "coin.lua" or "enemies/grunt.lua".
    pub path: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScriptWriteParams {
    /// Script path relative to `assets/scripts/`, ending in `.lua`, e.g.
    /// "coin.lua". Missing parent directories are created.
    pub path: String,
    /// The complete Lua 5.4 source (at most 256 KiB). It replaces the whole
    /// file; it is compiled before anything is written.
    pub source: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScriptCheckParams {
    /// Lua 5.4 source to compile (nothing is saved or run).
    pub source: String,
    /// Name shown in error messages, e.g. "coin.lua" (default "check.lua").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub path: Option<String>,
}

/// Why `path` cannot be a script path, or `None` when it looks valid.
#[must_use]
pub fn script_path_problem(path: &str) -> Option<String> {
    let p = path.trim();
    if p.is_empty() {
        return Some(
            "`path` is empty; give e.g. \"coin.lua\" (relative to assets/scripts/).".into(),
        );
    }
    if p.starts_with('/') || p.contains('\\') || p.contains(':') {
        return Some(format!(
            "`path` \"{p}\" must be relative to assets/scripts/ with `/` separators, e.g. \
             \"enemies/grunt.lua\"."
        ));
    }
    if let Some(rest) = p.strip_prefix("assets/scripts/") {
        return Some(format!(
            "`path` is already relative to assets/scripts/; drop the prefix: \"{rest}\"."
        ));
    }
    if p.split('/')
        .any(|seg| seg.is_empty() || seg == "." || seg == "..")
    {
        return Some(format!(
            "`path` \"{p}\" has an empty, \".\" or \"..\" segment; scripts stay inside \
             assets/scripts/."
        ));
    }
    // Exactly ".lua" (lower case), as the engine requires.
    if std::path::Path::new(p)
        .extension()
        .and_then(std::ffi::OsStr::to_str)
        != Some("lua")
    {
        return Some(format!("`path` \"{p}\" must end in \".lua\"."));
    }
    None
}

/// Why `source` cannot be saved, or `None`.
#[must_use]
pub fn script_source_problem(source: &str) -> Option<String> {
    if source.len() > MAX_SCRIPT_BYTES {
        return Some(format!(
            "`source` is {} bytes; scripts are at most {MAX_SCRIPT_BYTES} bytes. Split it into \
             modules loaded with require().",
            source.len()
        ));
    }
    if source.trim().is_empty() {
        return Some("`source` is empty; give the whole Lua file.".into());
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_are_checked_like_the_engine_does() {
        assert_eq!(script_path_problem("coin.lua"), None);
        assert_eq!(script_path_problem("enemies/grunt-1.lua"), None);
        for bad in [
            "",
            "/abs.lua",
            "../x.lua",
            "a//b.lua",
            "a/./b.lua",
            "c:\\x.lua",
            "notes.txt",
        ] {
            assert!(script_path_problem(bad).is_some(), "{bad}");
        }
        let prefixed = script_path_problem("assets/scripts/coin.lua").unwrap();
        assert!(prefixed.contains("\"coin.lua\""), "{prefixed}");
    }

    #[test]
    fn sources_are_size_limited() {
        assert_eq!(script_source_problem("return {}"), None);
        assert!(script_source_problem("  \n").is_some());
        assert!(script_source_problem(&"x".repeat(MAX_SCRIPT_BYTES + 1)).is_some());
    }
}
