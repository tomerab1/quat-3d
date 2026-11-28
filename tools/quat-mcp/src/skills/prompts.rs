//! MCP prompts generated from the skills, for clients without skills
//! support. Each skill `<name>` has a template `content/prompts/<name>.md`:
//! YAML frontmatter declares the prompt's `title` and `arguments`, and the
//! `MiniJinja` body frames the request and places the skill's instructions
//! with `{{ skill }}`. The prompt's description is the skill's.

use std::sync::LazyLock;

use minijinja::{Environment, UndefinedBehavior};
use rmcp::ErrorData;
use rmcp::model::{GetPromptResult, JsonObject, Prompt, PromptArgument, PromptMessage, Role};
use serde::Deserialize;
use serde_json::Value;

use super::embedded::{EmbeddedFile, PROMPT_FILES};
use super::{Catalog, SkillError, catalog, frontmatter};

/// Template variable that receives the skill's instructions.
pub const SKILL_VARIABLE: &str = "skill";

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct Header {
    #[serde(default)]
    title: Option<String>,
    #[serde(default)]
    arguments: Vec<ArgumentSpec>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct ArgumentSpec {
    name: String,
    description: String,
    #[serde(default)]
    required: bool,
}

/// One prompt: a skill and the template that frames it.
#[derive(Debug, Clone)]
struct SkillPrompt {
    name: String,
    title: Option<String>,
    description: String,
    arguments: Vec<ArgumentSpec>,
    skill: &'static str,
}

/// The prompts, one per skill that has a template.
#[derive(Debug)]
pub struct Prompts {
    prompts: Vec<SkillPrompt>,
    templates: Environment<'static>,
}

impl Prompts {
    /// Pair every skill of `catalog` with its template in `files`
    /// (`<skill>.md`). A skill without a template, a template without a
    /// skill and a template that does not parse are reported and left out.
    #[must_use]
    pub fn load(catalog: &Catalog, files: &[EmbeddedFile]) -> (Self, Vec<SkillError>) {
        let mut templates = Environment::new();
        // A misspelt variable is a bug, not an empty string.
        templates.set_undefined_behavior(UndefinedBehavior::Strict);
        let mut prompts = Vec::new();
        let mut errors = Vec::new();
        let error = |skill: &str, reason: String| SkillError {
            skill: skill.to_owned(),
            reason,
        };
        for file in files {
            let name = file.path.strip_suffix(".md").unwrap_or(file.path);
            let Some(skill) = catalog.by_name(name) else {
                errors.push(error(name, format!("prompts/{} has no skill", file.path)));
                continue;
            };
            let parsed = frontmatter::split(file.text).and_then(|(yaml, body)| {
                frontmatter::parse::<Header>(yaml).map(|header| (header, body))
            });
            let (header, body) = match parsed {
                Ok(parsed) => parsed,
                Err(e) => {
                    errors.push(error(name, format!("prompts/{}: {e}", file.path)));
                    continue;
                }
            };
            if let Err(e) = templates.add_template(name, body) {
                errors.push(error(name, format!("prompts/{}: {e}", file.path)));
                continue;
            }
            prompts.push(SkillPrompt {
                name: name.to_owned(),
                title: header.title,
                description: skill.description().to_owned(),
                arguments: header.arguments,
                skill: skill.body(),
            });
        }
        for skill in catalog.skills() {
            if !prompts.iter().any(|p| p.name == skill.name()) {
                errors.push(error(skill.name(), "no prompt template".to_owned()));
            }
        }
        (Self { prompts, templates }, errors)
    }

    /// The answer to `prompts/list`.
    #[must_use]
    pub fn list(&self) -> Vec<Prompt> {
        self.prompts
            .iter()
            .map(|p| {
                let arguments = p
                    .arguments
                    .iter()
                    .map(|a| {
                        PromptArgument::new(&a.name)
                            .with_description(&a.description)
                            .with_required(a.required)
                    })
                    .collect();
                let prompt = Prompt::new(&p.name, Some(&p.description), Some(arguments));
                match &p.title {
                    Some(title) => prompt.with_title(title),
                    None => prompt,
                }
            })
            .collect()
    }

    /// The answer to `prompts/get`: the rendered template as one user
    /// message.
    ///
    /// # Errors
    /// Invalid params for an unknown prompt, an unknown or non-string
    /// argument, or a missing required one.
    pub fn get(
        &self,
        name: &str,
        arguments: Option<&JsonObject>,
    ) -> Result<GetPromptResult, ErrorData> {
        let prompt = self
            .prompts
            .iter()
            .find(|p| p.name == name)
            .ok_or_else(|| {
                ErrorData::invalid_params(
                    format!("unknown prompt `{name}`; prompts: {}", self.names()),
                    None,
                )
            })?;
        let invalid = |message: String| ErrorData::invalid_params(message, None);
        let empty = JsonObject::new();
        let given = arguments.unwrap_or(&empty);
        if let Some(unknown) = given
            .keys()
            .find(|k| !prompt.arguments.iter().any(|a| &a.name == *k))
        {
            return Err(invalid(format!(
                "prompt `{name}` has no argument `{unknown}`; arguments: {}",
                argument_names(prompt)
            )));
        }
        let mut context = JsonObject::new();
        for spec in &prompt.arguments {
            let value = match given.get(&spec.name) {
                None | Some(Value::Null) => None,
                Some(Value::String(s)) => Some(s.trim()).filter(|s| !s.is_empty()),
                Some(_) => {
                    return Err(invalid(format!(
                        "prompt `{name}`: argument `{}` must be a string",
                        spec.name
                    )));
                }
            };
            if spec.required && value.is_none() {
                return Err(invalid(format!(
                    "prompt `{name}` needs the argument `{}`: {}",
                    spec.name, spec.description
                )));
            }
            context.insert(spec.name.clone(), value.map_or(Value::Null, Value::from));
        }
        context.insert(SKILL_VARIABLE.to_owned(), Value::from(prompt.skill));
        let text = self
            .templates
            .get_template(name)
            .and_then(|t| t.render(&context))
            .map_err(|e| ErrorData::internal_error(format!("prompt {name}: {e}"), None))?;
        Ok(
            GetPromptResult::new(vec![PromptMessage::new_text(Role::User, text)])
                .with_description(&prompt.description),
        )
    }

    fn names(&self) -> String {
        self.prompts
            .iter()
            .map(|p| p.name.as_str())
            .collect::<Vec<_>>()
            .join(", ")
    }
}

fn argument_names(prompt: &SkillPrompt) -> String {
    if prompt.arguments.is_empty() {
        return "none".to_owned();
    }
    prompt
        .arguments
        .iter()
        .map(|a| a.name.as_str())
        .collect::<Vec<_>>()
        .join(", ")
}

/// The prompts compiled into this binary.
#[must_use]
pub fn prompts() -> &'static Prompts {
    static PROMPTS: LazyLock<Prompts> = LazyLock::new(|| {
        let (prompts, errors) = Prompts::load(catalog(), PROMPT_FILES);
        for error in errors {
            tracing::error!("prompt for {error}");
        }
        prompts
    });
    &PROMPTS
}
