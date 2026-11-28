//! Model-facing text. It lives as Markdown under `content/` (see
//! `content/README.md`) and is embedded at compile time; tool descriptions
//! are attached directly to the tool functions with `#[doc = include_str!(..)]`.
//! Skills and their prompt templates are loaded by `crate::skills`.

use std::sync::LazyLock;

use minijinja::{Environment, UndefinedBehavior, context};
use serde::Serialize;

use crate::server::Workflows;
use crate::skills::SkillDoc;

/// MCP server instructions template, rendered by [`instructions`] and sent
/// to the client on `initialize`.
const INSTRUCTIONS: &str = include_str!("../content/instructions.md");

/// The Lua scripting reference served as the `quat://docs/scripting`
/// resource — the engine's own docs/scripting.md, so there is one source.
pub const SCRIPTING_REFERENCE: &str = include_str!("../../../docs/scripting.md");

/// Templates rendered with `MiniJinja`: the instructions and the files the
/// server writes.
const DOCUMENTS: &[(&str, &str)] = &[
    ("instructions", INSTRUCTIONS),
    (
        "asset_license",
        include_str!("../content/assets/license.md"),
    ),
];

static TEMPLATES: LazyLock<Environment<'static>> = LazyLock::new(|| {
    let mut env = Environment::new();
    // A misspelt variable is a bug, not an empty string.
    env.set_undefined_behavior(UndefinedBehavior::Strict);
    for (name, source) in DOCUMENTS {
        // Templates are compiled into the binary; `templates_compile` below
        // proves they parse, so a failure here cannot ship.
        if let Err(e) = env.add_template(name, source) {
            tracing::error!("template {name} does not parse: {e}");
        }
    }
    env
});

/// Render the document template `name` (see `DOCUMENTS`) with `args`.
///
/// # Errors
/// The template does not exist or refers to an argument `args` lacks.
pub fn render_document<S: Serialize>(name: &str, args: S) -> Result<String, minijinja::Error> {
    TEMPLATES.get_template(name)?.render(args)
}

/// The server instructions, listing the workflow `skills` and how this
/// server offers them.
#[must_use]
pub fn instructions(skills: &[SkillDoc], workflows: Workflows) -> String {
    let skills: Vec<_> = skills
        .iter()
        .map(|s| context! { name => s.name(), uri => s.uri() })
        .collect();
    let args = context! {
        skills,
        serve_skills => workflows.skills,
        serve_prompts => workflows.prompts,
    };
    // `instructions_render` below proves the template renders.
    render_document("instructions", args).unwrap_or_else(|e| {
        tracing::error!("server instructions do not render: {e}");
        INSTRUCTIONS.to_owned()
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::skills::catalog;
    use serde_json::json;

    #[test]
    fn templates_compile() {
        for (name, source) in DOCUMENTS {
            let mut env = Environment::new();
            env.add_template(name, source)
                .unwrap_or_else(|e| panic!("{name}: {e}"));
        }
    }

    #[test]
    fn instructions_render() {
        let skills = catalog().skills();
        for (s, p) in [(true, true), (true, false), (false, true), (false, false)] {
            let text = instructions(
                skills,
                Workflows {
                    skills: s,
                    prompts: p,
                },
            );
            assert!(!text.contains("{{") && !text.contains("{%"), "{text}");
            for skill in skills {
                assert!(text.contains(&format!("`{}`", skill.name())), "{text}");
                assert_eq!(text.contains(skill.uri()), s, "{text}");
            }
            assert_eq!(text.contains("prompts of the same name"), p, "{text}");
        }
    }

    #[test]
    fn missing_arguments_are_errors() {
        assert!(render_document("asset_license", json!({})).is_err());
        assert!(render_document("no_such_document", json!({})).is_err());
    }
}
