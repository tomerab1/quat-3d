//! Inputs of the material-shader tools (`shader_*`, docs/remote-control.md
//! "Material shaders"), and the shape of the engine's compile outcome.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Longest material shader name the engine accepts.
pub const MAX_SHADER_NAME: usize = 48;

/// Largest material shader source the engine accepts, in bytes.
pub const MAX_SHADER_SOURCE: usize = 256 * 1024;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShaderNameParams {
    /// Material shader name: lowercase letters, digits and `_`, starting with
    /// a letter (it is the file name under assets/shaders/materials/).
    /// `standard` is the built-in material.
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ShaderSourceParams {
    /// Material shader name: lowercase letters, digits and `_`, starting with
    /// a letter, at most 48 characters, e.g. `lava` or `brushed_metal`.
    pub name: String,
    /// The complete Slang module text (start from `shader_template`): it must
    /// `import material;`, implement `IMaterial` and end with
    /// `export struct SurfaceMaterial : IMaterial = <YourStruct>;`.
    pub source: String,
}

/// One compiler message, as the engine reports it.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Diagnostic {
    pub severity: String,
    pub message: String,
    #[serde(default)]
    pub file: Option<String>,
    #[serde(default)]
    pub line: Option<u32>,
    #[serde(default)]
    pub column: Option<u32>,
    #[serde(default)]
    pub code: Option<u32>,
    #[serde(default)]
    pub source_line: Option<String>,
}

/// `shader.create` / `shader.update`'s reply.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompileOutcome {
    pub name: String,
    #[serde(default)]
    pub path: String,
    pub ok: bool,
    #[serde(default)]
    pub kept_previous: bool,
    #[serde(default)]
    pub diagnostics: Vec<Diagnostic>,
    #[serde(default)]
    pub log: String,
}

/// Why a name cannot be a material shader name, or `None` when it can.
#[must_use]
pub fn shader_name_problem(name: &str) -> Option<String> {
    let valid_chars = name
        .bytes()
        .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_');
    let starts_with_letter = name.bytes().next().is_some_and(|b| b.is_ascii_lowercase());
    if name.is_empty() || name.len() > MAX_SHADER_NAME || !valid_chars || !starts_with_letter {
        return Some(format!(
            "`{name}` is not a material shader name: use lowercase letters, digits and `_`, \
             starting with a letter, at most {MAX_SHADER_NAME} characters (e.g. \"lava\")."
        ));
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_follow_the_engine_rule() {
        for ok in ["lava", "brushed_metal", "a1", "x"] {
            assert!(shader_name_problem(ok).is_none(), "{ok}");
        }
        for bad in ["", "Lava", "1st", "a-b", "../x", "a.b", &"a".repeat(49)] {
            assert!(shader_name_problem(bad).is_some(), "{bad}");
        }
    }

    #[test]
    fn outcomes_parse_with_and_without_locations() {
        let v = serde_json::json!({
            "name": "lava", "path": "assets/shaders/materials/lava.slang", "ok": false,
            "errors": 1, "kept_previous": true, "log": "...",
            "diagnostics": [
                {"severity": "error", "message": "'uvv' is not a member", "file": "a.slang",
                 "line": 9, "column": 33, "code": 30027, "source_line": "  x = input.uvv;"},
                {"severity": "note", "message": "the module must end with ..."}
            ]
        });
        let o: CompileOutcome = serde_json::from_value(v).unwrap();
        assert!(!o.ok && o.kept_previous);
        assert_eq!(o.diagnostics[0].line, Some(9));
        assert_eq!(o.diagnostics[1].file, None);
    }
}
