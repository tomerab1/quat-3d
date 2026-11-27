//! Workflow prompts. Their text is in `content/prompts/<prompt>.md`.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{PromptMessage, Role};
use rmcp::{ErrorData, prompt, prompt_router};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use super::QuatServer;
use crate::content::render_prompt;

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct BuildWorldArgs {
    /// What to build, e.g. "a small medieval village at sunset".
    pub description: String,
    /// Extra constraints, e.g. "keep the existing terrain" or "under 200 entities".
    #[serde(default)]
    pub constraints: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct InspectSceneArgs {
    /// What to focus on, e.g. "lighting", "physics setup", or an entity name.
    #[serde(default)]
    pub focus: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct DebugSessionArgs {
    /// The symptom to diagnose, e.g. "the crate falls through the floor" or "NPCs stand still".
    #[serde(default)]
    pub problem: Option<String>,
    /// An entity (id or name) to start from, if one is involved.
    #[serde(default)]
    pub entity: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct WriteMaterialShaderArgs {
    /// The look to create, e.g. "glowing lava with slowly flowing cracks" or "brushed metal".
    pub goal: String,
    /// An entity (id or name) to put the material on, if any.
    #[serde(default)]
    pub entity: Option<String>,
}

/// `Some(trimmed)` for non-blank text.
fn non_blank(text: Option<String>) -> Option<String> {
    text.map(|t| t.trim().to_owned()).filter(|t| !t.is_empty())
}

fn user_prompt(name: &str, args: impl Serialize) -> Result<Vec<PromptMessage>, ErrorData> {
    let text = render_prompt(name, args)
        .map_err(|e| ErrorData::internal_error(format!("prompt {name}: {e}"), None))?;
    Ok(vec![PromptMessage::new_text(Role::User, text)])
}

#[prompt_router(vis = "pub(super)")]
impl QuatServer {
    /// Build a 3D world in the running quat-3d engine from a description: inspect, plan, build with batches, screenshot, iterate.
    #[prompt]
    async fn build_world(
        &self,
        args: Parameters<BuildWorldArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let BuildWorldArgs {
            description,
            constraints,
        } = args.0;
        user_prompt(
            "build_world",
            BuildWorldArgs {
                description: description.trim().to_owned(),
                constraints: non_blank(constraints),
            },
        )
    }

    /// Inspect the running quat-3d scene and report its structure, lighting, physics and any problems.
    #[prompt]
    async fn inspect_scene(
        &self,
        args: Parameters<InspectSceneArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        user_prompt(
            "inspect_scene",
            InspectSceneArgs {
                focus: non_blank(args.0.focus),
            },
        )
    }

    /// Diagnose a problem in the running quat-3d game like a debugger: stats, logs, watch entities, pause and single-step the simulation, raycast, query navmesh paths.
    #[prompt]
    async fn debug_session(
        &self,
        args: Parameters<DebugSessionArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let DebugSessionArgs { problem, entity } = args.0;
        user_prompt(
            "debug_session",
            DebugSessionArgs {
                problem: non_blank(problem),
                entity: non_blank(entity),
            },
        )
    }

    /// Write a material shader (a Slang module the engine compiles and hot-swaps) for a look, put it on an entity, and iterate from compiler errors and screenshots.
    #[prompt]
    async fn write_material_shader(
        &self,
        args: Parameters<WriteMaterialShaderArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let WriteMaterialShaderArgs { goal, entity } = args.0;
        user_prompt(
            "write_material_shader",
            WriteMaterialShaderArgs {
                goal: goal.trim().to_owned(),
                entity: non_blank(entity),
            },
        )
    }
}
