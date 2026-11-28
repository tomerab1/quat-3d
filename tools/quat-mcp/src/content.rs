//! Model-facing text. It lives as Markdown under `content/` (see
//! `content/README.md`) and is embedded at compile time; tool descriptions
//! are attached directly to the tool functions with `#[doc = include_str!(..)]`.

use std::sync::LazyLock;

use minijinja::{Environment, UndefinedBehavior};
use serde::Serialize;

/// MCP server instructions, sent to the client on `initialize`.
pub const INSTRUCTIONS: &str = include_str!("../content/instructions.md");

/// The Lua scripting reference served as the `quat://docs/scripting`
/// resource — the engine's own docs/scripting.md, so there is one source.
pub const SCRIPTING_REFERENCE: &str = include_str!("../../../docs/scripting.md");

/// Prompt templates by name.
const PROMPTS: &[(&str, &str)] = &[
    (
        "build_world",
        include_str!("../content/prompts/build_world.md"),
    ),
    (
        "inspect_scene",
        include_str!("../content/prompts/inspect_scene.md"),
    ),
    (
        "debug_session",
        include_str!("../content/prompts/debug_session.md"),
    ),
    ("tune_look", include_str!("../content/prompts/tune_look.md")),
    (
        "design_behavior",
        include_str!("../content/prompts/design_behavior.md"),
    ),
    (
        "write_game_script",
        include_str!("../content/prompts/write_game_script.md"),
    ),
    (
        "find_performance_problem",
        include_str!("../content/prompts/find_performance_problem.md"),
    ),
    (
        "write_material_shader",
        include_str!("../content/prompts/write_material_shader.md"),
    ),
];

/// Templates for files the server writes (rendered with the same engine).
const DOCUMENTS: &[(&str, &str)] = &[(
    "asset_license",
    include_str!("../content/assets/license.md"),
)];

static TEMPLATES: LazyLock<Environment<'static>> = LazyLock::new(|| {
    let mut env = Environment::new();
    // A misspelt variable is a bug, not an empty string.
    env.set_undefined_behavior(UndefinedBehavior::Strict);
    for (name, source) in PROMPTS.iter().chain(DOCUMENTS) {
        // Templates are compiled into the binary; `templates_compile` below
        // proves they parse, so a failure here cannot ship.
        if let Err(e) = env.add_template(name, source) {
            tracing::error!("prompt template {name} does not parse: {e}");
        }
    }
    env
});

/// Render the prompt template `name` with `args`.
///
/// # Errors
/// The template does not exist or refers to an argument `args` lacks.
pub fn render_prompt<S: Serialize>(name: &str, args: S) -> Result<String, minijinja::Error> {
    TEMPLATES.get_template(name)?.render(args)
}

/// Render the document template `name` (see `DOCUMENTS`) with `args`.
///
/// # Errors
/// The template does not exist or refers to an argument `args` lacks.
pub fn render_document<S: Serialize>(name: &str, args: S) -> Result<String, minijinja::Error> {
    render_prompt(name, args)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn templates_compile() {
        for (name, source) in PROMPTS.iter().chain(DOCUMENTS) {
            let mut env = Environment::new();
            env.add_template(name, source)
                .unwrap_or_else(|e| panic!("{name}: {e}"));
        }
    }

    #[test]
    fn optional_blocks_render_only_when_given() {
        let with = render_prompt(
            "build_world",
            json!({"description": "a harbour", "constraints": "under 50 entities"}),
        )
        .unwrap();
        assert!(with.starts_with(
            "Build this in the running quat-3d engine using the quat-mcp tools: a harbour\n\n\
             Constraints: under 50 entities\n"
        ));

        let without = render_prompt(
            "build_world",
            json!({"description": "a harbour", "constraints": null}),
        )
        .unwrap();
        assert!(!without.contains("Constraints"));
        assert!(!without.contains("{{") && !without.contains("{%"));
    }

    #[test]
    fn missing_arguments_are_errors() {
        assert!(render_prompt("build_world", json!({})).is_err());
        assert!(render_prompt("no_such_prompt", json!({})).is_err());
    }
}
