//! Structured output for the stable tools: typed result shapes (their
//! schemas are the tools' `outputSchema`) and the check that turns an engine
//! reply into `structuredContent`. Shapes follow `docs/remote-control.md`;
//! fields the engine may add later are allowed, so only a removed or
//! retyped field breaks the contract (and `reply_matches` reports it).

use std::collections::BTreeMap;
use std::sync::{Arc, LazyLock};

use rmcp::handler::server::tool::schema_for_output;
use rmcp::model::JsonObject;
use schemars::JsonSchema;
use serde::Deserialize;
use serde::de::DeserializeOwned;
use serde_json::Value;

/// `engine_info`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct EngineInfo {
    /// Always "quat-3d".
    pub engine: String,
    /// Frames rendered since start.
    pub frame: u64,
    /// Live entities.
    pub entities: u64,
    /// In play mode.
    pub playing: bool,
    /// Simulation paused (`debug_pause`).
    #[serde(default)]
    pub paused: Option<bool>,
    /// Running with the editor.
    pub editor: bool,
}

/// One entity in a `scene_list` page.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct EntitySummary {
    /// Entity id.
    pub id: u64,
    /// Entity name (unique names can be used instead of ids).
    pub name: String,
    /// Parent id, or null at the root.
    pub parent: Option<u64>,
    /// Component names.
    pub components: Vec<String>,
    /// World position [x, y, z] in metres.
    pub world_position: [f64; 3],
    /// World-space bounds of the subtree `{min, max}`, or null.
    pub world_bounds: Option<Value>,
}

/// `scene_list`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct SceneList {
    /// Entities matching the filter (before `limit` / `offset`).
    pub total: u64,
    /// This page.
    pub entities: Vec<EntitySummary>,
}

/// `scene_create`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct Created {
    /// The new entity's id.
    pub id: u64,
    /// Its name.
    pub name: String,
}

/// `scene_destroy` and `scene_clear`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct Destroyed {
    /// Entities removed, children included.
    pub destroyed: u64,
}

/// `scene_save` and `scene_load`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct SceneFile {
    /// Project-relative path of the scene file.
    pub path: String,
}

/// `world_ground_height`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct GroundHeights {
    /// Height per point, null where no terrain tile is loaded.
    pub heights: Vec<Option<f64>>,
    /// Tiles are still generating; retry for the nulls.
    pub generating: bool,
}

/// `terrain_status`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct TerrainStatus {
    /// The scene has terrain; the other fields come only when true.
    pub present: bool,
    /// Terrain entity id.
    #[serde(default)]
    pub entity: Option<u64>,
    /// Tiles stream around the camera.
    #[serde(default)]
    pub streaming: Option<bool>,
    /// Tiles are still generating.
    #[serde(default)]
    pub generating: Option<bool>,
    /// Loaded tiles.
    #[serde(default)]
    pub tiles_loaded: Option<u64>,
    /// Tiles wanted around the anchor.
    #[serde(default)]
    pub tiles_wanted: Option<u64>,
    /// Streaming anchor [x, z].
    #[serde(default)]
    pub anchor: Option<Value>,
    /// Tile edge in metres.
    #[serde(default)]
    pub tile_size: Option<f64>,
    /// Loaded bounds `{min, max}`, or null.
    #[serde(default)]
    pub bounds: Option<Value>,
}

/// `physics_get_gravity` and `physics_set_gravity`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct Gravity {
    /// Gravity [x, y, z] in m/s².
    pub gravity: [f64; 3],
    /// Its length in m/s².
    pub magnitude: f64,
}

/// One script in `script_list`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct ScriptEntry {
    /// Path under assets/scripts/.
    pub path: String,
    /// Size in bytes.
    pub bytes: u64,
    /// Entities whose `script` component uses it.
    pub entities: Value,
    /// Load / runtime status, with errors as `file:line`.
    #[serde(default)]
    pub status: Option<Value>,
}

/// `script_list`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct ScriptList {
    /// Every script.
    pub scripts: Vec<ScriptEntry>,
    /// The script runtime is running (play mode).
    pub running: bool,
}

/// One material shader in `shader_list`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct ShaderEntry {
    /// Shader name.
    pub name: String,
    /// Built-in (cannot be edited or deleted).
    pub builtin: bool,
    /// Source path.
    pub path: String,
    /// Compile state.
    pub state: String,
    /// Successful hot reloads.
    pub reloads: u64,
    /// Errors of the last compile.
    pub errors: Value,
}

/// `shader_list`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct ShaderList {
    /// Every material shader.
    pub shaders: Vec<ShaderEntry>,
    /// The engine can compile shaders (slangc available).
    pub can_compile: bool,
}

/// `events_poll`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct EventsPage {
    /// Events after `since`, oldest first: `{seq, kind, time_s, entity_a, entity_b, ...}`.
    pub events: Vec<Value>,
    /// Pass as `since` next time.
    pub cursor: u64,
    /// More events are waiting.
    pub more: bool,
    /// Events lost between `since` and `cursor`.
    pub dropped: u64,
    /// Oldest sequence number still readable.
    pub oldest_seq: u64,
    /// Newest sequence number.
    pub last_seq: u64,
    /// Ring size.
    pub capacity: u64,
}

/// `project_build_status`.
#[derive(Debug, Deserialize, JsonSchema)]
pub struct BuildStatus {
    /// Build job id.
    pub job: u64,
    /// running, succeeded, failed.
    pub state: String,
    /// 0..1.
    pub progress: f64,
    /// Current step number.
    pub step: u64,
    /// Number of steps.
    pub steps: u64,
    /// Current step.
    pub step_name: String,
    /// Package archive (project-relative), when done.
    #[serde(default)]
    pub package: Option<String>,
    /// Package folder (project-relative), when done.
    #[serde(default)]
    pub folder: Option<String>,
    /// Why it failed.
    #[serde(default)]
    pub error: Option<String>,
    /// Seconds since the build started.
    pub elapsed_s: f64,
    /// Log tail.
    pub log: Vec<String>,
}

/// A stable tool's output contract.
struct Output {
    schema: Arc<JsonObject>,
    check: fn(&Value) -> Result<(), String>,
}

fn check<T: DeserializeOwned>(v: &Value) -> Result<(), String> {
    T::deserialize(v).map(|_| ()).map_err(|e| e.to_string())
}

fn output<T: JsonSchema + DeserializeOwned + 'static>() -> Output {
    Output {
        schema: schema_for_output::<T>(),
        check: check::<T>,
    }
}

static OUTPUTS: LazyLock<BTreeMap<&'static str, Output>> = LazyLock::new(|| {
    BTreeMap::from([
        ("engine_info", output::<EngineInfo>()),
        ("scene_list", output::<SceneList>()),
        ("scene_create", output::<Created>()),
        ("scene_destroy", output::<Destroyed>()),
        ("scene_clear", output::<Destroyed>()),
        ("scene_save", output::<SceneFile>()),
        ("scene_load", output::<SceneFile>()),
        ("world_ground_height", output::<GroundHeights>()),
        ("terrain_status", output::<TerrainStatus>()),
        ("physics_get_gravity", output::<Gravity>()),
        ("physics_set_gravity", output::<Gravity>()),
        ("script_list", output::<ScriptList>()),
        ("shader_list", output::<ShaderList>()),
        ("events_poll", output::<EventsPage>()),
        ("project_build_status", output::<BuildStatus>()),
    ])
});

/// The `outputSchema` of tool `name`, if it has a stable result.
#[must_use]
pub fn schema(name: &str) -> Option<Arc<JsonObject>> {
    OUTPUTS.get(name).map(|o| Arc::clone(&o.schema))
}

/// Whether tool `name` declares an output schema.
#[must_use]
pub fn has_schema(name: &str) -> bool {
    OUTPUTS.contains_key(name)
}

/// Check an engine reply against tool `name`'s contract.
///
/// # Errors
/// The reply lacks a field of the contract or has one of the wrong type.
pub fn reply_matches(name: &str, reply: &Value) -> Result<(), String> {
    OUTPUTS.get(name).map_or(Ok(()), |o| (o.check)(reply))
}

/// Tools with an output schema.
pub fn names() -> impl Iterator<Item = &'static str> {
    OUTPUTS.keys().copied()
}

#[cfg(test)]
mod tests {
    use serde_json::json;

    use super::*;

    #[test]
    fn schemas_are_objects() {
        for name in names() {
            let schema = schema(name).expect("schema");
            assert_eq!(schema.get("type"), Some(&json!("object")), "{name}");
            assert!(schema.get("properties").is_some(), "{name}");
        }
    }

    #[test]
    fn replies_are_checked_but_extra_fields_pass() {
        let info = json!({"engine":"quat-3d","frame":1,"entities":2,"playing":false,
                          "editor":true,"future_field":1});
        assert!(reply_matches("engine_info", &info).is_ok());
        let missing = json!({"engine":"quat-3d","frame":1});
        let e = reply_matches("engine_info", &missing).unwrap_err();
        assert!(e.contains("entities"), "{e}");
        assert!(reply_matches("terrain_status", &json!({"present": false})).is_ok());
        assert!(reply_matches("no_such_tool", &json!(null)).is_ok());
    }
}
