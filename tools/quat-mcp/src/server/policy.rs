//! Per-tool policy: one row per tool holding everything the protocol says
//! about a tool besides its description and input schema — the title, the
//! behaviour hints (read-only / destructive / idempotent / open-world), the
//! icon group, whether results can be large, and whether a call needs the
//! user's confirmation. [`apply`] writes it onto the router's tool
//! definitions; the rule tests below keep the table complete and coherent.
//!
//! Every hint is set explicitly: the MCP defaults for an absent hint are the
//! pessimistic ones (`destructiveHint: true`, `openWorldHint: true`), so a
//! tool that only said `idempotentHint` was shown as a destructive
//! open-world tool.

use std::collections::BTreeMap;
use std::sync::{Arc, LazyLock};

use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::model::{JsonObject, MetaObject, ToolAnnotations};
use serde_json::{Value, json};

use super::icons::{self, Group};
use super::outputs;

/// `_meta` key Claude clients read on a tool definition: the largest result,
/// in characters, they keep inline instead of truncating or spilling to a
/// file.
pub const MAX_RESULT_SIZE_KEY: &str = "anthropic/maxResultSizeChars";
/// The limit given to tools whose results grow with the scene (lists, logs,
/// sources, schemas, profiles).
pub const LARGE_RESULT_CHARS: u64 = 400_000;
/// Name of the argument that confirms a destructive call when the client
/// cannot ask the user (no elicitation support).
pub const CONFIRM_ARG: &str = "confirm";

/// What a tool does to engine or project state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Effect {
    /// Reads only (`readOnlyHint: true`).
    Read,
    /// Changes state additively or reversibly.
    Write,
    /// May delete or overwrite (`destructiveHint: true`).
    Destructive,
}

/// How large a tool's results can get.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Results {
    /// Bounded and small.
    Small,
    /// Grow with the scene and can exceed a client's default inline size.
    Large,
}

/// The protocol-level facts about one tool.
#[derive(Debug, Clone, Copy)]
pub struct Policy {
    pub name: &'static str,
    pub title: &'static str,
    pub group: Group,
    pub effect: Effect,
    pub idempotent: bool,
    /// Reaches outside the local engine and project (the web).
    pub open_world: bool,
    /// How large results can get.
    pub results: Results,
    /// Asks the user before running (elicitation, or `confirm: true`).
    pub confirm: bool,
}

const fn row(
    name: &'static str,
    title: &'static str,
    group: Group,
    effect: Effect,
    idempotent: bool,
) -> Policy {
    Policy {
        name,
        title,
        group,
        effect,
        idempotent,
        open_world: false,
        results: Results::Small,
        confirm: false,
    }
}

/// A read-only tool: never destructive, always idempotent.
const fn read(name: &'static str, title: &'static str, group: Group) -> Policy {
    row(name, title, group, Effect::Read, true)
}

/// A tool that changes state without deleting or overwriting anything.
const fn write(name: &'static str, title: &'static str, group: Group, idem: bool) -> Policy {
    row(name, title, group, Effect::Write, idem)
}

/// A tool that may delete or overwrite.
const fn destroy(name: &'static str, title: &'static str, group: Group, idem: bool) -> Policy {
    row(name, title, group, Effect::Destructive, idem)
}

impl Policy {
    const fn large(mut self) -> Self {
        self.results = Results::Large;
        self
    }

    const fn open_world(mut self) -> Self {
        self.open_world = true;
        self
    }

    const fn confirm(mut self) -> Self {
        self.confirm = true;
        self
    }

    /// The annotations this policy puts on the tool definition.
    #[must_use]
    pub fn annotations(&self) -> ToolAnnotations {
        ToolAnnotations::with_title(self.title)
            .read_only(self.effect == Effect::Read)
            .destructive(self.effect == Effect::Destructive)
            .idempotent(self.idempotent)
            .open_world(self.open_world)
    }
}

use Group::{
    Asset, Behavior, Camera, Code, Debug, Engine, Physics, Project, Render, Scene, Sim, World,
};

/// Every tool's policy. A tool missing here, or a row for a tool that does
/// not exist, fails `every_tool_has_exactly_one_policy`.
pub const POLICIES: &[Policy] = &[
    // engine
    read("engine_info", "Engine status", Engine),
    read("engine_methods", "Engine methods", Engine).large(),
    destroy("engine_call", "Raw engine call", Engine, false).large(),
    // scene
    read("scene_components", "Component types", Scene).large(),
    read("scene_list", "List entities", Scene).large(),
    read("scene_get", "Inspect entity", Scene).large(),
    write("scene_create", "Create entity", Scene, false),
    write("scene_update", "Update entity", Scene, true),
    destroy("scene_remove_components", "Remove components", Scene, true),
    destroy("scene_destroy", "Destroy entity", Scene, true),
    write("scene_set_parent", "Re-parent entity", Scene, true),
    destroy("scene_clear", "Clear scene", Scene, true).confirm(),
    destroy("scene_batch", "Batch scene edits", Scene, false).large(),
    destroy("scene_save", "Save scene file", Scene, true),
    destroy("scene_load", "Load scene file", Scene, true).confirm(),
    // world: sky, weather, clock, terrain
    read("environment_get", "Read sky", World),
    write("environment_set", "Set sky", World, true),
    read("weather_get", "Read weather", World),
    write("weather_set", "Set weather", World, true),
    read("time_get", "Read world clock", World),
    write("time_set", "Set world clock", World, true),
    write("time_skip", "Skip world time", World, false),
    read("world_ground_height", "Ground height", World),
    write("world_place_on_ground", "Place on ground", World, true),
    read("terrain_status", "Terrain status", World),
    // camera and views
    read("camera_get", "Read camera", Camera),
    write("camera_set", "Move camera", Camera, true),
    write("camera_frame", "Frame entities", Camera, true),
    read("camera_rig_get", "Read camera rig", Camera),
    write("camera_rig_set", "Drive camera rig", Camera, true),
    write("camera_rig_frame", "Frame with camera rig", Camera, true),
    read("screenshot", "Screenshot", Camera),
    // play mode, input, navigation
    write("sim_play", "Play", Sim, true),
    write("sim_stop", "Stop", Sim, true),
    write("nav_bake", "Bake navmesh", Sim, true),
    read("input_actions", "Input actions", Sim),
    write("input_inject", "Inject input", Sim, false),
    write("input_bind", "Bind input", Sim, true),
    write("input_context", "Push/pop input context", Sim, false),
    // debugging and profiling
    read("debug_log", "Engine log", Debug).large(),
    read("debug_stats", "Engine stats", Debug),
    write("debug_pause", "Pause simulation", Debug, true),
    write("debug_resume", "Resume simulation", Debug, true),
    write("debug_step", "Step simulation", Debug, false),
    write("debug_time_scale", "Time scale", Debug, true),
    read("debug_watch", "Watch entities", Debug).large(),
    read("debug_raycast", "Debug raycast", Debug),
    read("debug_nav_path", "Navmesh path", Debug),
    read("profile_frame", "Frame profile", Debug).large(),
    write("profile_capture", "Capture profile", Debug, false).large(),
    read("profile_counters", "Profile counters", Debug),
    // behaviour trees
    read("bt_leaves", "Behaviour-tree leaves", Behavior).large(),
    read("bt_get", "Read behaviour tree", Behavior).large(),
    destroy("bt_set", "Replace behaviour tree", Behavior, true),
    write("bt_add_node", "Add tree node", Behavior, false),
    destroy("bt_remove_node", "Remove tree node", Behavior, false),
    write("bt_move_node", "Move tree node", Behavior, false),
    write("bt_set_params", "Set node parameters", Behavior, true),
    read("bt_status", "Behaviour-tree status", Behavior).large(),
    // physics
    read("events_poll", "Physics events", Physics).large(),
    read("physics_get", "Read body", Physics),
    write("physics_set", "Set body state", Physics, true),
    write("physics_apply", "Apply impulse / force", Physics, false),
    read("physics_get_gravity", "Read gravity", Physics),
    write("physics_set_gravity", "Set gravity", Physics, true),
    read("physics_raycast_all", "Raycast (all hits)", Physics),
    read("physics_overlap", "Overlap query", Physics),
    read("physics_shape_cast", "Shape cast", Physics),
    write("character_move", "Move character", Physics, false),
    // scripts and shaders
    read("script_list", "List scripts", Code),
    read("script_get", "Read script", Code).large(),
    write("script_create", "Create script", Code, false),
    destroy("script_update", "Update script", Code, true),
    destroy("script_delete", "Delete script", Code, true).confirm(),
    read("script_check", "Check script", Code),
    read("shader_list", "List material shaders", Code),
    read("shader_get", "Read material shader", Code).large(),
    read("shader_template", "Material shader template", Code).large(),
    write("shader_create", "Create material shader", Code, false),
    destroy("shader_update", "Update material shader", Code, true),
    destroy("shader_delete", "Delete material shader", Code, true).confirm(),
    // look: renderer settings and materials
    read("render_settings_get", "Read render settings", Render),
    write("render_settings_set", "Set render settings", Render, true),
    destroy(
        "render_settings_reset",
        "Reset render settings",
        Render,
        true,
    ),
    read("render_settings_schema", "Render settings schema", Render).large(),
    read("material_get", "Read material", Render),
    write("material_set", "Set material", Render, true),
    // assets
    read("asset_list_models", "List models", Asset),
    write("asset_spawn_model", "Spawn model", Asset, false),
    read("asset_search", "Search Poly Haven", Asset).open_world(),
    write("asset_fetch", "Download from Poly Haven", Asset, true).open_world(),
    // project
    read("project_info", "Project info", Project),
    destroy("project_build", "Build game package", Project, false),
    read("project_build_status", "Build status", Project).large(),
];

static BY_NAME: LazyLock<BTreeMap<&'static str, &'static Policy>> =
    LazyLock::new(|| POLICIES.iter().map(|p| (p.name, p)).collect());

/// The policy of tool `name`.
#[must_use]
pub fn get(name: &str) -> Option<&'static Policy> {
    BY_NAME.get(name).copied()
}

/// Write the policies onto `router`'s tool definitions: annotations, icons,
/// the result-size hint, output schemas, and the `confirm` argument of the
/// tools that ask first.
pub fn apply<S>(router: &mut ToolRouter<S>) {
    for route in router.map.values_mut() {
        let tool = &mut route.attr;
        let Some(policy) = get(&tool.name) else {
            // `every_tool_has_exactly_one_policy` makes this unreachable.
            tracing::error!("tool {} has no policy row", tool.name);
            continue;
        };
        tool.title = Some(policy.title.to_owned());
        tool.annotations = Some(policy.annotations());
        tool.icons = Some(icons::icons(policy.group).to_vec());
        if policy.results == Results::Large {
            let mut meta = tool.meta.take().unwrap_or_default();
            meta.insert(MAX_RESULT_SIZE_KEY.to_owned(), json!(LARGE_RESULT_CHARS));
            tool.meta = Some(meta);
        }
        if let Some(schema) = outputs::schema(policy.name) {
            tool.output_schema = Some(schema);
        }
        if policy.confirm {
            tool.input_schema = with_confirm_arg(&tool.input_schema);
        }
    }
}

/// `schema` plus an optional boolean `confirm` property.
fn with_confirm_arg(schema: &Arc<JsonObject>) -> Arc<JsonObject> {
    let mut schema = schema.as_ref().clone();
    let properties = schema
        .entry("properties")
        .or_insert_with(|| Value::Object(JsonObject::new()));
    if let Value::Object(properties) = properties {
        properties.insert(
            CONFIRM_ARG.to_owned(),
            json!({
                "type": "boolean",
                "description": "Only when the client cannot ask the user (no elicitation): \
                                true after the user agreed to this destructive call.",
            }),
        );
    }
    Arc::new(schema)
}

/// The `_meta` a tool definition carries, for tests.
#[must_use]
pub fn result_size_hint(meta: Option<&MetaObject>) -> Option<u64> {
    meta?.get(MAX_RESULT_SIZE_KEY)?.as_u64()
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;

    use super::*;
    use crate::server::QuatServer;

    #[test]
    fn every_tool_has_exactly_one_policy() {
        let tools: BTreeSet<String> = QuatServer::tools()
            .list_all()
            .into_iter()
            .map(|t| t.name.to_string())
            .collect();
        let rows: BTreeSet<String> = POLICIES.iter().map(|p| p.name.to_owned()).collect();
        assert_eq!(rows.len(), POLICIES.len(), "duplicate policy rows");
        assert_eq!(tools, rows, "tools vs policy rows");
    }

    /// The annotation audit: every hint explicit, and coherent with the
    /// tool's name and with each other.
    #[test]
    fn annotations_follow_the_rules() {
        for tool in QuatServer::tools().list_all() {
            let name = tool.name.as_ref();
            let a = tool.annotations.as_ref().expect("annotations");
            let title = a.title.as_deref().expect("annotation title");
            assert_eq!(tool.title.as_deref(), Some(title), "{name}");
            let (Some(ro), Some(de), Some(id), Some(ow)) = (
                a.read_only_hint,
                a.destructive_hint,
                a.idempotent_hint,
                a.open_world_hint,
            ) else {
                panic!("{name}: every hint must be explicit, got {a:?}");
            };
            if ro {
                assert!(!de && id, "{name}: read-only tools are safe and idempotent");
            }
            let reads = ["_get", "_list", "_status", "_schema", "_template", "_check"];
            let reads_prefix = ["engine_info", "engine_methods", "scene_components"];
            if reads.iter().any(|s| name.ends_with(s)) || reads_prefix.contains(&name) {
                assert!(ro, "{name}: getters and lists are read-only");
            }
            let deletes = [
                "_delete",
                "_destroy",
                "_clear",
                "_remove_components",
                "_load",
            ];
            if deletes.iter().any(|s| name.ends_with(s)) || name.contains("_remove_") {
                assert!(de, "{name}: deleting tools are destructive");
            }
            assert_eq!(
                ow,
                name.starts_with("asset_search") || name.starts_with("asset_fetch"),
                "{name}: only the Poly Haven tools reach outside the engine"
            );
        }
    }

    #[test]
    fn only_destructive_tools_ask_for_confirmation() {
        let confirm: BTreeSet<&str> = POLICIES
            .iter()
            .filter(|p| p.confirm)
            .map(|p| p.name)
            .collect();
        for p in POLICIES.iter().filter(|p| p.confirm) {
            assert_eq!(p.effect, Effect::Destructive, "{}", p.name);
        }
        // Whole-scene and file deletions ask; per-entity edits do not.
        assert_eq!(
            confirm,
            BTreeSet::from([
                "scene_clear",
                "scene_load",
                "script_delete",
                "shader_delete"
            ])
        );
        for tool in QuatServer::tools().list_all() {
            let has_arg = tool
                .input_schema
                .get("properties")
                .and_then(|p| p.get(CONFIRM_ARG))
                .is_some();
            assert_eq!(
                has_arg,
                confirm.contains(tool.name.as_ref()),
                "{}",
                tool.name
            );
        }
    }

    #[test]
    fn large_results_carry_the_size_hint() {
        for tool in QuatServer::tools().list_all() {
            let policy = get(&tool.name).expect("policy");
            let hint = result_size_hint(tool.meta.as_ref());
            assert_eq!(
                hint,
                (policy.results == Results::Large).then_some(LARGE_RESULT_CHARS),
                "{}",
                tool.name
            );
        }
    }

    #[test]
    fn every_tool_has_light_and_dark_icons() {
        for tool in QuatServer::tools().list_all() {
            let icons = tool.icons.as_ref().expect("icons");
            assert_eq!(icons.len(), 2, "{}", tool.name);
            for icon in icons {
                assert!(icon.src.starts_with("data:image/svg+xml;base64,"));
                assert_eq!(icon.mime_type.as_deref(), Some("image/svg+xml"));
                assert!(icon.theme.is_some());
            }
        }
    }
}
