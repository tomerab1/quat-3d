//! YAML frontmatter of `SKILL.md` and prompt-template files, and the Agent
//! Skills rules on it (<https://agentskills.io/specification>).

use serde::de::DeserializeOwned;
use serde_json::{Map, Value};

/// Longest skill `name` (Agent Skills specification).
pub const NAME_MAX: usize = 64;
/// Longest skill `description`.
pub const DESCRIPTION_MAX: usize = 1024;
/// Longest skill `compatibility`.
pub const COMPATIBILITY_MAX: usize = 500;

const FENCE: &str = "---";

/// Why a file's frontmatter cannot be used.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum FrontmatterError {
    #[error("the file does not start with a `---` frontmatter block")]
    Missing,
    #[error("the frontmatter block has no closing `---` line")]
    Unclosed,
    #[error("invalid frontmatter: {0}")]
    Yaml(String),
    #[error("{0}")]
    Rule(String),
}

/// Split a file into its YAML frontmatter and the Markdown body after it.
///
/// # Errors
/// The text does not start with a `---` line or the block is never closed.
pub fn split(text: &str) -> Result<(&str, &str), FrontmatterError> {
    let rest = text
        .strip_prefix(FENCE)
        .and_then(|r| r.strip_prefix('\n').or_else(|| r.strip_prefix("\r\n")))
        .ok_or(FrontmatterError::Missing)?;
    let mut offset = 0;
    for line in rest.split_inclusive('\n') {
        if line.trim_end_matches(['\r', '\n']) == FENCE {
            let body = &rest[offset + line.len()..];
            return Ok((&rest[..offset], body.trim_start_matches(['\r', '\n'])));
        }
        offset += line.len();
    }
    Err(FrontmatterError::Unclosed)
}

/// Parse a frontmatter block into `T`.
///
/// # Errors
/// The YAML is malformed or does not have the shape of `T`.
pub fn parse<T: DeserializeOwned>(yaml: &str) -> Result<T, FrontmatterError> {
    yaml_serde::from_str(yaml).map_err(|e| FrontmatterError::Yaml(e.to_string()))
}

/// Check a `SKILL.md` frontmatter against the Agent Skills specification:
/// `name` (lowercase letters, digits and single hyphens, equal to the skill's
/// directory), `description`, and the optional `compatibility` and
/// `metadata` (a map of strings).
///
/// # Errors
/// The first rule the frontmatter breaks.
pub fn validate(fields: &Map<String, Value>, directory: &str) -> Result<(), FrontmatterError> {
    let rule = |message: String| Err(FrontmatterError::Rule(message));
    let name = fields
        .get("name")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if !is_valid_name(name) {
        return rule(format!(
            "name `{name}`: expected 1-{NAME_MAX} lowercase letters, digits and single \
             hyphens, not starting or ending with a hyphen"
        ));
    }
    if name != directory {
        return rule(format!(
            "name `{name}` does not match its directory `{directory}`"
        ));
    }
    let description = fields
        .get("description")
        .and_then(Value::as_str)
        .unwrap_or_default();
    if description.trim().is_empty() || description.chars().count() > DESCRIPTION_MAX {
        return rule(format!(
            "description: expected 1-{DESCRIPTION_MAX} characters, got {}",
            description.chars().count()
        ));
    }
    if let Some(compatibility) = fields.get("compatibility") {
        let ok = compatibility
            .as_str()
            .is_some_and(|c| (1..=COMPATIBILITY_MAX).contains(&c.chars().count()));
        if !ok {
            return rule(format!(
                "compatibility: expected 1-{COMPATIBILITY_MAX} characters of text"
            ));
        }
    }
    if let Some(metadata) = fields.get("metadata") {
        let ok = metadata
            .as_object()
            .is_some_and(|m| m.values().all(Value::is_string));
        if !ok {
            return rule("metadata: expected a map of string values".to_owned());
        }
    }
    Ok(())
}

/// An Agent Skills `name`: 1-64 of `a-z`, `0-9` and `-`, no leading,
/// trailing or doubled hyphen.
#[must_use]
pub fn is_valid_name(name: &str) -> bool {
    (1..=NAME_MAX).contains(&name.len())
        && name
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        && !name.starts_with('-')
        && !name.ends_with('-')
        && !name.contains("--")
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn splits_frontmatter_from_body() {
        let (yaml, body) = split("---\nname: a\n---\n\n# Title\n").unwrap();
        assert_eq!(yaml, "name: a\n");
        assert_eq!(body, "# Title\n");
        let (yaml, body) = split("---\r\nname: a\r\n---\r\nbody").unwrap();
        assert_eq!(yaml, "name: a\r\n");
        assert_eq!(body, "body");
        assert_eq!(split("# no frontmatter"), Err(FrontmatterError::Missing));
        assert_eq!(split("---\nname: a\n"), Err(FrontmatterError::Unclosed));
        // A `---` inside a value is not the closing fence.
        let (yaml, _) = split("---\ndescription: a --- b\n---\nx").unwrap();
        assert_eq!(yaml, "description: a --- b\n");
    }

    #[test]
    fn names_follow_the_agent_skills_rules() {
        for ok in ["a", "build-level", "r2d2", &"x".repeat(64)] {
            assert!(is_valid_name(ok), "{ok}");
        }
        for bad in [
            "",
            "Build",
            "-a",
            "a-",
            "a--b",
            "a_b",
            "a b",
            &"x".repeat(65),
        ] {
            assert!(!is_valid_name(bad), "{bad}");
        }
    }

    #[test]
    fn validates_fields() {
        let fields = |v: Value| v.as_object().cloned().unwrap();
        let good = fields(json!({"name": "tune-look", "description": "Tune it.",
            "compatibility": "Needs an engine.", "metadata": {"k": "v"}}));
        assert_eq!(validate(&good, "tune-look"), Ok(()));
        let err = |v: Value, dir: &str| validate(&fields(v), dir).unwrap_err().to_string();
        assert!(
            err(json!({"name": "tune-look", "description": "x"}), "other").contains("directory")
        );
        assert!(err(json!({"name": "Tune", "description": "x"}), "Tune").contains("lowercase"));
        assert!(err(json!({"name": "a", "description": ""}), "a").contains("description"));
        assert!(err(json!({"name": "a", "description": "x".repeat(1025)}), "a").contains("1025"));
        assert!(
            err(
                json!({"name": "a", "description": "x", "compatibility": ""}),
                "a"
            )
            .contains("compatibility")
        );
        assert!(
            err(
                json!({"name": "a", "description": "x", "metadata": {"k": 1}}),
                "a"
            )
            .contains("metadata")
        );
    }

    #[test]
    fn parses_yaml_into_json() {
        let v: Map<String, Value> = parse("name: a\nmetadata:\n  k: \"1\"\n").unwrap();
        assert_eq!(
            Value::Object(v),
            json!({"name": "a", "metadata": {"k": "1"}})
        );
        assert!(matches!(
            parse::<Map<String, Value>>("a: [1"),
            Err(FrontmatterError::Yaml(_))
        ));
    }
}
