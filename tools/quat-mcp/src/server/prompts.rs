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
pub struct TuneLookArgs {
    /// The look to reach, e.g. "warm late-afternoon light, less washed out" or "glossier car paint".
    #[serde(default)]
    pub goal: Option<String>,
    /// An entity (id or name) whose material matters most, if any.
    #[serde(default)]
    pub entity: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct DesignBehaviorArgs {
    /// What the NPC should do, e.g. "patrol between the gates and chase the player when near".
    pub goal: String,
    /// The entity (id or name) to give the behaviour, if it already exists.
    #[serde(default)]
    pub entity: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct WriteGameScriptArgs {
    /// What the script should do, e.g. "coins that spin and add to a score when the player touches them".
    pub goal: String,
    /// The entity (id or name) to attach the script to, if it exists already.
    #[serde(default)]
    pub entity: Option<String>,
    /// File to write, relative to assets/scripts/, e.g. "coins/coin.lua".
    #[serde(default)]
    pub path: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
pub struct FindPerformanceProblemArgs {
    /// What is slow, e.g. "stutters when the forest streams in" or "30 fps in the editor".
    #[serde(default)]
    pub symptom: Option<String>,
    /// The frame rate to reach, e.g. 60 (sets the spike budget to 1000 / fps ms).
    #[serde(default)]
    pub target_fps: Option<u32>,
}

/// What the `find_performance_problem` template is rendered with: the
/// arguments plus the frame budget derived from `target_fps`.
#[derive(Serialize)]
struct PerformanceContext {
    symptom: Option<String>,
    target_fps: Option<u32>,
    budget_ms: Option<f64>,
}

/// The frame budget in milliseconds for `fps` (two decimals), if positive.
fn budget_ms(fps: Option<u32>) -> Option<f64> {
    fps.filter(|f| *f > 0)
        .map(|f| (100_000.0 / f64::from(f)).round() / 100.0)
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

    /// Tune how the running quat-3d scene looks from screenshots: exposure first, then tonemap/bloom, shadows and sky light, then materials, one change at a time.
    #[prompt]
    async fn tune_look(
        &self,
        args: Parameters<TuneLookArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let TuneLookArgs { goal, entity } = args.0;
        user_prompt(
            "tune_look",
            TuneLookArgs {
                goal: non_blank(goal),
                entity: non_blank(entity),
            },
        )
    }

    /// Design an NPC behaviour tree in the running quat-3d engine: look up the leaves, plan the tree, build it, play, watch what it does, iterate.
    #[prompt]
    async fn design_behavior(
        &self,
        args: Parameters<DesignBehaviorArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let DesignBehaviorArgs { goal, entity } = args.0;
        user_prompt(
            "design_behavior",
            DesignBehaviorArgs {
                goal: goal.trim().to_owned(),
                entity: non_blank(entity),
            },
        )
    }

    /// Find why the running quat-3d engine is slow with the profiler: capture a baseline, classify CPU- or GPU-bound, locate the costly passes or systems, test one fix at a time.
    #[prompt]
    async fn find_performance_problem(
        &self,
        args: Parameters<FindPerformanceProblemArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let FindPerformanceProblemArgs {
            symptom,
            target_fps,
        } = args.0;
        let target_fps = target_fps.filter(|f| *f > 0);
        user_prompt(
            "find_performance_problem",
            PerformanceContext {
                symptom: non_blank(symptom),
                target_fps,
                budget_ms: budget_ms(target_fps),
            },
        )
    }

    /// Write gameplay as a Lua script in the running quat-3d game: read the scripting reference, write and compile, attach, play, read the log, iterate.
    #[prompt]
    async fn write_game_script(
        &self,
        args: Parameters<WriteGameScriptArgs>,
    ) -> Result<Vec<PromptMessage>, ErrorData> {
        let WriteGameScriptArgs { goal, entity, path } = args.0;
        user_prompt(
            "write_game_script",
            WriteGameScriptArgs {
                goal: goal.trim().to_owned(),
                entity: non_blank(entity),
                path: non_blank(path),
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
