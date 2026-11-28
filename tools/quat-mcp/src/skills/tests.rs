//! Sync tests: the skills, the tools and resources they name, the prompts
//! generated from them and the plugin's copy stay consistent with each other
//! and with the files on disk.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};

use rmcp::model::{ErrorCode, SkillResources};
use serde_json::{Value, json};

use super::embedded::{EmbeddedFile, PROMPT_FILES, SKILL_FILES};
use super::prompts::{Prompts, SKILL_VARIABLE};
use super::{Catalog, TOOLS_KEY, catalog};
use crate::server::{QuatServer, SCENE_SUMMARY_URI, SCRIPTING_REFERENCE_URI};

/// Tool groups still in flight on other branches (profiling,
/// physics control, events, input injection): a skill must not name their
/// tools before they exist here.
///
/// Limitation: a body mention counts as a tool only when its prefix belongs
/// to an existing tool group or to this list, so a skill naming a tool of a
/// brand-new group (say `audio_play`) passes unnoticed. Add each new group's
/// prefix here as soon as it is planned.
const PENDING_TOOL_PREFIXES: &[&str] = &[
    "profile",
    "physics",
    "events",
    "input",
    "character",
    "joint",
];

/// Inline-code identifiers in skills that look like tool names but are
/// fields or components.
const NOT_TOOLS: &[&str] = &[
    "debug_cascades",
    "local_bounds",
    "nav_agent",
    "nav_agent_state",
    "shader_params",
    "weather_auto",
    "weather_preset",
    "world_bounds",
    "world_position",
];

/// `SKILL.md` line budget (Agent Skills progressive-disclosure guidance).
const SKILL_MD_MAX_LINES: usize = 500;

fn manifest_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
}

/// Every file below `dir`, by `/`-separated relative path.
fn files_under(dir: &Path) -> BTreeMap<String, Vec<u8>> {
    fn walk(root: &Path, dir: &Path, out: &mut BTreeMap<String, Vec<u8>>) {
        for entry in std::fs::read_dir(dir).unwrap_or_else(|e| panic!("{}: {e}", dir.display())) {
            let path = entry.unwrap().path();
            if path.is_dir() {
                walk(root, &path, out);
            } else {
                let relative = path.strip_prefix(root).unwrap();
                let key = relative
                    .components()
                    .map(|c| c.as_os_str().to_string_lossy().into_owned())
                    .collect::<Vec<_>>()
                    .join("/");
                out.insert(key, std::fs::read(&path).unwrap());
            }
        }
    }
    let mut out = BTreeMap::new();
    walk(dir, dir, &mut out);
    out
}

fn embedded_paths(files: &[EmbeddedFile]) -> BTreeSet<String> {
    files.iter().map(|f| f.path.to_owned()).collect()
}

fn tool_names() -> BTreeSet<String> {
    QuatServer::tools()
        .list_all()
        .into_iter()
        .map(|t| t.name.to_string())
        .collect()
}

/// Inline code spans of `markdown`, outside fenced code blocks.
fn inline_code(markdown: &str) -> Vec<&str> {
    let mut spans = Vec::new();
    let mut fenced = false;
    for line in markdown.lines() {
        if line.trim_start().starts_with("```") {
            fenced = !fenced;
            continue;
        }
        if !fenced {
            spans.extend(line.split('`').skip(1).step_by(2));
        }
    }
    spans
}

/// The tools a skill's text names: inline code that is a tool's name, or
/// an identifier in a tool group's namespace (`scene_`, `debug_`, ...).
fn mentioned_tools(markdown: &str, tools: &BTreeSet<String>) -> BTreeSet<String> {
    let prefixes: BTreeSet<&str> = tools
        .iter()
        .filter_map(|t| t.split('_').next())
        .chain(PENDING_TOOL_PREFIXES.iter().copied())
        .collect();
    inline_code(markdown)
        .into_iter()
        .filter(|span| {
            let identifier = span.contains('_')
                && span
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_');
            tools.contains(*span)
                || (identifier
                    && !NOT_TOOLS.contains(span)
                    && span.split('_').next().is_some_and(|p| prefixes.contains(p)))
        })
        .map(str::to_owned)
        .collect()
}

#[test]
fn embedded_files_match_the_content_directories() {
    let content = manifest_dir().join("content");
    let on_disk: BTreeSet<String> = files_under(&content.join("skills")).into_keys().collect();
    assert_eq!(
        embedded_paths(SKILL_FILES),
        on_disk,
        "skills::embedded::SKILL_FILES vs content/skills/"
    );
    let on_disk: BTreeSet<String> = files_under(&content.join("prompts")).into_keys().collect();
    assert_eq!(
        embedded_paths(PROMPT_FILES),
        on_disk,
        "skills::embedded::PROMPT_FILES vs content/prompts/"
    );
}

#[test]
fn every_skill_loads_and_follows_the_format() {
    let (loaded, errors) = Catalog::load(SKILL_FILES);
    assert!(errors.is_empty(), "{errors:#?}");
    let directories: BTreeSet<&str> = SKILL_FILES
        .iter()
        .filter_map(|f| f.path.split('/').next())
        .collect();
    assert_eq!(loaded.skills().len(), directories.len());
    for skill in loaded.skills() {
        assert!(directories.contains(skill.name()), "{}", skill.name());
        assert_eq!(skill.uri(), format!("skill://{}/SKILL.md", skill.name()));
        skill.entry().validate().unwrap();
        let lines = skill.body().lines().count();
        assert!(
            lines < SKILL_MD_MAX_LINES,
            "{}: {lines} lines",
            skill.name()
        );
        assert!(
            skill.body().starts_with("# "),
            "{}: body starts with a title",
            skill.name()
        );
    }
}

#[test]
fn skills_name_only_existing_tools_and_declare_them() {
    let tools = tool_names();
    for skill in catalog().skills() {
        let name = skill.name();
        let declared: BTreeSet<String> = skill.tools().iter().cloned().collect();
        assert!(
            !declared.is_empty(),
            "{name}: metadata.{TOOLS_KEY} is empty"
        );
        assert_eq!(
            declared.len(),
            skill.tools().len(),
            "{name}: a tool is listed twice"
        );
        let unknown: Vec<_> = declared.difference(&tools).collect();
        assert!(
            unknown.is_empty(),
            "{name} declares tools quat-mcp lacks: {unknown:?}"
        );

        let mentioned = mentioned_tools(skill.body(), &tools);
        let not_tools: Vec<_> = mentioned.difference(&tools).collect();
        assert!(
            not_tools.is_empty(),
            "{name} names tools quat-mcp lacks: {not_tools:?} (a field? add it to NOT_TOOLS)"
        );
        assert_eq!(
            mentioned, declared,
            "{name}: tools in the text vs metadata.{TOOLS_KEY}"
        );
    }
}

#[test]
fn skills_reference_only_served_resources_and_skills() {
    let resources = [SCENE_SUMMARY_URI, SCRIPTING_REFERENCE_URI];
    let names: BTreeSet<&str> = catalog()
        .skills()
        .iter()
        .map(super::SkillDoc::name)
        .collect();
    for skill in catalog().skills() {
        let body = skill.body();
        for (at, _) in body.match_indices("quat://") {
            let uri: String = body[at..]
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric() || "/:_.-".contains(*c))
                .collect();
            let uri = uri.trim_end_matches('.');
            assert!(
                resources.contains(&uri),
                "{}: unknown resource {uri}",
                skill.name()
            );
        }
        // "the tune-look skill": a hyphenated word before "skill" is a skill name.
        let words: Vec<&str> = body.split_whitespace().collect();
        for pair in words.windows(2) {
            let candidate = pair[0].trim_matches('`');
            if pair[1].starts_with("skill")
                && candidate.contains('-')
                && candidate
                    .bytes()
                    .all(|b| b.is_ascii_lowercase() || b == b'-')
            {
                assert!(
                    names.contains(candidate),
                    "{}: unknown skill {candidate}",
                    skill.name()
                );
            }
        }
    }
}

#[test]
fn every_skill_has_one_prompt() {
    let (prompts, errors) = Prompts::load(catalog(), PROMPT_FILES);
    assert!(errors.is_empty(), "{errors:#?}");
    let prompt_names: Vec<String> = prompts.list().into_iter().map(|p| p.name).collect();
    let skill_names: Vec<String> = catalog()
        .skills()
        .iter()
        .map(|s| s.name().to_owned())
        .collect();
    assert_eq!(
        prompt_names.iter().collect::<BTreeSet<_>>(),
        skill_names.iter().collect::<BTreeSet<_>>()
    );
    for prompt in prompts.list() {
        let skill = catalog().by_name(&prompt.name).unwrap();
        assert_eq!(prompt.description.as_deref(), Some(skill.description()));
        assert!(prompt.title.is_some(), "{}: no title", prompt.name);
    }
}

#[test]
fn prompts_render_the_skill_with_and_without_optional_arguments() {
    let (prompts, _) = Prompts::load(catalog(), PROMPT_FILES);
    for prompt in prompts.list() {
        let arguments = prompt.arguments.clone().unwrap_or_default();
        let all: serde_json::Map<String, Value> = arguments
            .iter()
            .map(|a| (a.name.clone(), json!(format!("<{}>", a.name))))
            .collect();
        let required: serde_json::Map<String, Value> = all
            .iter()
            .filter(|(k, _)| {
                arguments
                    .iter()
                    .any(|a| &a.name == *k && a.required == Some(true))
            })
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect();
        let skill = catalog().by_name(&prompt.name).unwrap();
        for given in [&all, &required] {
            let result = prompts.get(&prompt.name, Some(given)).unwrap();
            let text = result.messages[0].content.as_text().unwrap().text.clone();
            assert!(
                text.contains(skill.body().trim()),
                "{}: skill missing",
                prompt.name
            );
            assert!(
                !text.contains("{{") && !text.contains("{%"),
                "{}",
                prompt.name
            );
            for (key, value) in &all {
                let shown = text.contains(value.as_str().unwrap());
                assert_eq!(shown, given.contains_key(key), "{}: {key}", prompt.name);
            }
        }
    }
    // Each template places the skill's instructions once.
    let placeholder = format!("{{{{ {SKILL_VARIABLE} }}}}");
    for file in PROMPT_FILES {
        assert_eq!(file.text.matches(&placeholder).count(), 1, "{}", file.path);
    }
}

#[test]
fn prompt_arguments_are_validated() {
    let (prompts, _) = Prompts::load(catalog(), PROMPT_FILES);
    let args = |v: Value| v.as_object().cloned().unwrap();
    let invalid = |name: &str, given: Option<Value>| {
        let given = given.map(args);
        let error = prompts.get(name, given.as_ref()).unwrap_err();
        assert_eq!(error.code, ErrorCode::INVALID_PARAMS, "{name}");
        error.message.to_string()
    };
    assert!(invalid("build_world", None).contains("build-level"));
    assert!(invalid("build-level", None).contains("description"));
    assert!(invalid("build-level", Some(json!({"description": "  "}))).contains("description"));
    assert!(invalid("build-level", Some(json!({"description": 3}))).contains("string"));
    assert!(
        invalid(
            "build-level",
            Some(json!({"description": "a hut", "style": "x"}))
        )
        .contains("constraints")
    );
    let ok = prompts
        .get(
            "tune-look",
            Some(&args(json!({"goal": " warmer ", "entity": null}))),
        )
        .unwrap();
    let text = &ok.messages[0].content.as_text().unwrap().text;
    assert!(text.contains("The goal: warmer\n"), "{text}");
    assert!(!text.contains("Material to focus on"), "{text}");
}

#[test]
fn manifests_describe_the_served_bytes() {
    for skill in catalog().skills() {
        let SkillResources::Files(files) = &skill.entry().resources else {
            panic!("{}: dynamic", skill.name());
        };
        assert!(files.iter().any(|f| f.uri == skill.uri()));
        for file in files {
            let read = catalog().read(&file.uri).unwrap();
            let rmcp::model::ResourceContents::TextResourceContents {
                text, mime_type, ..
            } = &read.contents[0]
            else {
                panic!("{}: not text", file.uri);
            };
            file.verify(text.as_bytes()).unwrap();
            assert_eq!(mime_type.as_deref(), Some("text/markdown"));
        }
        let got = catalog().get_skill(skill.uri()).unwrap();
        assert_eq!(&got.skill, skill.entry());
    }
    let error = catalog().get_skill("skill://nope/SKILL.md").unwrap_err();
    assert_eq!(error.code, ErrorCode::INVALID_PARAMS);
    assert!(error.message.contains("skill://build-level/SKILL.md"));
    assert!(catalog().read("skill://nope/SKILL.md").is_none());
    assert_eq!(catalog().resources().len(), catalog().skills().len());
}

#[test]
fn a_broken_skill_is_reported_and_left_out() {
    let files = [
        EmbeddedFile {
            path: "good/SKILL.md",
            text: "---\nname: good\ndescription: Fine.\n---\n# Good\n",
        },
        EmbeddedFile {
            path: "good/references/notes.md",
            text: "notes\n",
        },
        EmbeddedFile {
            path: "renamed/SKILL.md",
            text: "---\nname: other\ndescription: Wrong directory.\n---\n# X\n",
        },
        EmbeddedFile {
            path: "headless/SKILL.md",
            text: "# No frontmatter\n",
        },
        EmbeddedFile {
            path: "lonely/notes.md",
            text: "no SKILL.md\n",
        },
    ];
    let (loaded, errors) = Catalog::load(&files);
    let names: Vec<&str> = loaded.skills().iter().map(super::SkillDoc::name).collect();
    assert_eq!(names, ["good"]);
    let broken: BTreeSet<&str> = errors.iter().map(|e| e.skill.as_str()).collect();
    assert_eq!(broken, BTreeSet::from(["headless", "lonely", "renamed"]));
    // Supporting files are in the manifest and readable.
    let SkillResources::Files(manifest) = &loaded.skills()[0].entry().resources else {
        panic!("dynamic");
    };
    assert_eq!(manifest.len(), 2);
    assert!(loaded.read("skill://good/references/notes.md").is_some());
    assert!(loaded.read("skill://renamed/SKILL.md").is_none());

    let (prompts, errors) = Prompts::load(&loaded, &[]);
    assert!(prompts.list().is_empty());
    assert_eq!(errors.len(), 1, "{errors:?}");
}

#[test]
fn plugin_skills_are_a_copy_of_the_content() {
    let root = manifest_dir();
    let content = files_under(&root.join("content/skills"));
    let plugin = files_under(&root.join("plugin/skills"));
    assert!(
        content == plugin,
        "plugin/skills/ differs from content/skills/; refresh the copy from tools/quat-mcp with \
         `rm -r plugin/skills && cp -R content/skills plugin/skills`"
    );
}

#[test]
fn plugin_and_marketplace_manifests_agree() {
    let root = manifest_dir();
    let read = |path: PathBuf| -> Value {
        let text =
            std::fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        serde_json::from_str(&text).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
    };
    let plugin = read(root.join("plugin/.claude-plugin/plugin.json"));
    assert_eq!(plugin["name"], "quat-3d");
    assert_eq!(plugin["version"], env!("CARGO_PKG_VERSION"));

    let servers = read(root.join("plugin/.mcp.json"));
    let server = &servers["quat-3d"];
    let args: Vec<&str> = server["args"]
        .as_array()
        .unwrap()
        .iter()
        .map(|a| a.as_str().unwrap())
        .collect();
    // The plugin ships the skills as files: its server serves neither copy.
    assert_eq!(args, ["--skills=false", "--prompts=false"]);
    let serialized = servers.to_string().to_lowercase();
    assert!(!serialized.contains("token"), "no secrets in the plugin");
    for option in ["quat_mcp_command", "engine_port", "project_root"] {
        assert!(
            serialized.contains(&format!("user_config.{option}")),
            "{option}"
        );
        assert!(
            plugin["userConfig"][option]["default"].is_string(),
            "{option}"
        );
    }

    let repo = root.join("../..");
    let marketplace = read(repo.join(".claude-plugin/marketplace.json"));
    let entry = marketplace["plugins"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["name"] == "quat-3d")
        .expect("marketplace lists quat-3d");
    let source = entry["source"].as_str().unwrap();
    assert_eq!(source, "./tools/quat-mcp/plugin");
    assert!(
        repo.join(source)
            .join(".claude-plugin/plugin.json")
            .is_file()
    );
}
