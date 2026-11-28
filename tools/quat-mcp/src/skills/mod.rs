//! Agent Skills: the quat-3d workflows in `content/skills/<name>/SKILL.md`.
//!
//! One source feeds three surfaces:
//! - the MCP skills extension (SEP-2640, `io.modelcontextprotocol/skills`):
//!   `skills/list`, `skills/get`, and every skill file as a `skill://`
//!   resource ([`Catalog`]);
//! - MCP prompts of the same names for clients without skills
//!   ([`prompts`]);
//! - the Claude Code plugin in `plugin/`, whose `skills/` is a copy that the
//!   tests keep byte-identical.

mod embedded;
pub mod frontmatter;
pub mod prompts;
#[cfg(test)]
mod tests;

use std::collections::BTreeMap;
use std::sync::LazyLock;

use rmcp::ErrorData;
use rmcp::model::{
    CacheScope, GetSkillResult, ListSkillsResult, ReadResourceResult, Resource, ResourceContents,
    SKILL_MD, Skill, SkillFrontmatter, SkillResource, SkillResources,
};
use serde_json::{Map, Value};

use embedded::{EmbeddedFile, SKILL_FILES};

/// URI scheme of skill files.
pub const SCHEME: &str = "skill://";
/// Frontmatter `metadata` key listing the quat-mcp tools a skill uses,
/// separated by spaces.
pub const TOOLS_KEY: &str = "quat-mcp-tools";
/// Cache lifetime hinted for skill listings: the skills are compiled into the
/// binary and cannot change while it runs.
const TTL_MS: u64 = 60 * 60 * 1000;

/// A skill that cannot be served.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("skill `{skill}`: {reason}")]
pub struct SkillError {
    pub skill: String,
    pub reason: String,
}

/// One skill: its `skills/list` entry, its instructions and its tools.
#[derive(Debug, Clone)]
pub struct SkillDoc {
    entry: Skill,
    body: &'static str,
    tools: Vec<String>,
}

impl SkillDoc {
    #[must_use]
    pub fn name(&self) -> &str {
        &self.entry.frontmatter.name
    }

    /// `skill://<name>/SKILL.md`.
    #[must_use]
    pub fn uri(&self) -> &str {
        &self.entry.uri
    }

    #[must_use]
    pub fn description(&self) -> &str {
        &self.entry.frontmatter.description
    }

    /// The Markdown instructions after the frontmatter.
    #[must_use]
    pub fn body(&self) -> &'static str {
        self.body
    }

    /// The quat-mcp tools the skill names in its frontmatter (`TOOLS_KEY`).
    #[must_use]
    pub fn tools(&self) -> &[String] {
        &self.tools
    }

    /// The entry `skills/list` and `skills/get` return.
    #[must_use]
    pub fn entry(&self) -> &Skill {
        &self.entry
    }
}

/// The skills this server serves, and their files by URI.
#[derive(Debug, Default)]
pub struct Catalog {
    skills: Vec<SkillDoc>,
    files: BTreeMap<String, &'static str>,
}

impl Catalog {
    /// Build the catalog from `<skill>/<file>` paths. A skill that breaks a
    /// rule is left out and reported; the others are served.
    #[must_use]
    pub fn load(files: &[EmbeddedFile]) -> (Self, Vec<SkillError>) {
        let mut by_skill: BTreeMap<&str, Vec<&EmbeddedFile>> = BTreeMap::new();
        for file in files {
            let directory = file.path.split('/').next().unwrap_or_default();
            by_skill.entry(directory).or_default().push(file);
        }
        let mut catalog = Self::default();
        let mut errors = Vec::new();
        for (directory, skill_files) in by_skill {
            match load_skill(directory, &skill_files) {
                Ok(doc) => {
                    for file in skill_files {
                        catalog
                            .files
                            .insert(format!("{SCHEME}{}", file.path), file.text);
                    }
                    catalog.skills.push(doc);
                }
                Err(reason) => errors.push(SkillError {
                    skill: directory.to_owned(),
                    reason,
                }),
            }
        }
        (catalog, errors)
    }

    /// Every skill, ordered by name.
    #[must_use]
    pub fn skills(&self) -> &[SkillDoc] {
        &self.skills
    }

    #[must_use]
    pub fn by_name(&self, name: &str) -> Option<&SkillDoc> {
        self.skills.iter().find(|s| s.name() == name)
    }

    /// The answer to `skills/list`: every skill on one page.
    #[must_use]
    pub fn list_skills(&self) -> ListSkillsResult {
        ListSkillsResult::with_all_items(self.skills.iter().map(|s| s.entry.clone()).collect())
            .with_ttl_ms(TTL_MS)
            .with_cache_scope(CacheScope::Public)
    }

    /// The answer to `skills/get`.
    ///
    /// # Errors
    /// `-32602` (invalid params) when no skill has that URI, as the
    /// extension requires.
    pub fn get_skill(&self, uri: &str) -> Result<GetSkillResult, ErrorData> {
        let skill = self.skills.iter().find(|s| s.uri() == uri).ok_or_else(|| {
            ErrorData::invalid_params(
                format!("unknown skill `{uri}`; skills: {}", self.uris()),
                None,
            )
        })?;
        Ok(GetSkillResult::new(skill.entry.clone())
            .with_ttl_ms(TTL_MS)
            .with_cache_scope(CacheScope::Public))
    }

    /// The `SKILL.md` of every skill, for `resources/list`.
    #[must_use]
    pub fn resources(&self) -> Vec<Resource> {
        self.skills
            .iter()
            .map(|s| {
                Resource::new(s.uri(), s.name())
                    .with_description(s.description())
                    .with_mime_type(mime_type(s.uri()))
            })
            .collect()
    }

    /// The answer to `resources/read` for a skill file, or `None` when the
    /// URI is not one.
    #[must_use]
    pub fn read(&self, uri: &str) -> Option<ReadResourceResult> {
        let text = self.files.get(uri)?;
        Some(ReadResourceResult::new(vec![
            ResourceContents::text(*text, uri).with_mime_type(mime_type(uri)),
        ]))
    }

    /// The `SKILL.md` URIs, comma-separated (for error messages).
    #[must_use]
    pub fn uris(&self) -> String {
        self.skills
            .iter()
            .map(SkillDoc::uri)
            .collect::<Vec<_>>()
            .join(", ")
    }
}

/// The skills compiled into this binary.
#[must_use]
pub fn catalog() -> &'static Catalog {
    static CATALOG: LazyLock<Catalog> = LazyLock::new(|| {
        let (catalog, errors) = Catalog::load(SKILL_FILES);
        // The sync tests prove every shipped skill loads; this only fires
        // for a skill edited without running them.
        for error in errors {
            tracing::error!("not serving {error}");
        }
        catalog
    });
    &CATALOG
}

fn load_skill(directory: &str, files: &[&EmbeddedFile]) -> Result<SkillDoc, String> {
    let skill_md_path = format!("{directory}/{SKILL_MD}");
    let skill_md = files
        .iter()
        .find(|f| f.path == skill_md_path)
        .ok_or_else(|| format!("no {SKILL_MD}"))?;
    let (yaml, body) = frontmatter::split(skill_md.text).map_err(|e| e.to_string())?;
    let fields: Map<String, Value> = frontmatter::parse(yaml).map_err(|e| e.to_string())?;
    frontmatter::validate(&fields, directory).map_err(|e| e.to_string())?;
    let tools = fields
        .get("metadata")
        .and_then(|m| m.get(TOOLS_KEY))
        .and_then(Value::as_str)
        .map(|t| t.split_whitespace().map(str::to_owned).collect())
        .unwrap_or_default();
    let frontmatter: SkillFrontmatter = serde_json::from_value(Value::Object(fields))
        .map_err(|e| format!("invalid frontmatter: {e}"))?;

    let mut manifest: Vec<SkillResource> = files
        .iter()
        .map(|f| SkillResource::from_bytes(format!("{SCHEME}{}", f.path), f.text.as_bytes()))
        .collect();
    manifest.sort_by(|a, b| a.uri.cmp(&b.uri));
    let entry = Skill::new(
        format!("{SCHEME}{skill_md_path}"),
        frontmatter,
        SkillResources::Files(manifest),
    );
    entry.validate().map_err(|e| e.to_string())?;
    Ok(SkillDoc { entry, body, tools })
}

/// Media type of a skill file, by extension.
fn mime_type(uri: &str) -> &'static str {
    match uri.rsplit_once('.').map(|(_, ext)| ext) {
        Some("md") => "text/markdown",
        Some("json") => "application/json",
        Some("lua") => "text/x-lua",
        Some("yaml" | "yml") => "application/yaml",
        _ => "text/plain",
    }
}
