//! The MCP server: one tool per engine method (docs/remote-control.md, v1
//! contract), workflow prompts, and a scene-summary resource.

use std::sync::Arc;
use std::time::Duration;

use rmcp::handler::server::router::prompt::PromptRouter;
use rmcp::handler::server::router::tool::ToolRouter;
use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::{
    CallToolResult, ContentBlock, Implementation, ListResourcesResult, PaginatedRequestParams,
    PromptMessage, ReadResourceRequestParams, ReadResourceResponse, ReadResourceResult, Resource,
    ResourceContents, Role, ServerCapabilities, ServerConfig,
};
use rmcp::service::RequestContext;
use rmcp::{
    ErrorData, RoleServer, ServerHandler, prompt, prompt_handler, prompt_router, tool,
    tool_handler, tool_router,
};
use schemars::JsonSchema;
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};

use crate::client::{EngineClient, EngineError, code};
use crate::params::*;

/// Most `world_ground_height` points one call may carry (contract limit).
pub const MAX_GROUND_POINTS: usize = 1024;

/// URI of the scene-summary resource.
pub const SCENE_SUMMARY_URI: &str = "quat://scene/summary";

/// Component reference shared by the tools that take component payloads.
macro_rules! components_doc {
    () => {
        "\n\nCOMPONENTS — a JSON object keyed by component name. Updates merge: omitted fields keep \
their values. Unknown names/fields are rejected with the list of known ones. Units: metres, \
seconds, degrees; Y is up.\n\
- transform: position [x,y,z] (local, relative to the parent); rotation [x,y,z] Euler degrees \
applied in YXZ order (x = pitch, y = yaw/heading, z = roll); scale number or [x,y,z]; look_at \
[x,y,z] world point to face (an entity's forward is -Z). world_position is read-only.\n\
- primitive: shape cube|sphere|plane|cylinder|cone|capsule (about unit size, centred on the \
position — size it with transform.scale; a cube scaled [w,h,d] rests on y=0 at position y=h/2); \
color \"#rrggbb\" / \"#rrggbbaa\" sRGB hex or linear [r,g,b,a] (alpha < 1 = see-through); \
metallic 0..1; roughness 0..1; emissive linear [r,g,b] (HDR, > 1 glows).\n\
- point_light: color linear [r,g,b]; radius (range, m); intensity >= 0.\n\
- dir_light: direction [x,y,z] (the light's travel direction); color linear [r,g,b]; \
intensity. Prefer environment_set for the sun.\n\
- camera: fov_deg 1..179; near; far (> near); active.\n\
- collider: shape box|sphere|capsule; half_extents [x,y,z] (> 0); offset [x,y,z]; sensor \
(trigger volume, no collision response).\n\
- rigidbody: motion static|kinematic|dynamic; mass (kg). Pair with a collider.\n\
- character: radius; half_height; speed (m/s) — capsule character controller.\n\
- nav_agent: target [x,y,z]; speed (m/s); ground_offset; active. Needs a baked navmesh (nav_bake).\n\
- behavior: source (behaviour-tree JSON object or its text); enabled.\n\
- patrol: points [[x,y,z],...]; loop. Walked by the entity's nav_agent.\n\
- terrain: seed; resolution 17..8193; tile_size (m); height (m); octaves 1..16; warp 0..4; \
droplets (erosion, 0..50000000); snowline (m); streaming; stream_radius 0..8 (tiles).\n\
- net_replicated: net_id.\n\
- animator (edit only, present on animated glTF models): time (s); speed; looping.\n\
- mesh: read only (glTF provenance).\n\
scene_components lists what the running engine supports."
    };
}

const INSTRUCTIONS: &str = "quat-mcp drives a running quat-3d game engine (Vulkan renderer, \
Jolt physics, editor). Use it to build and inspect 3D worlds: create entities from primitive \
shapes, lights, physics bodies, terrain and glTF models; set the sun, clouds and camera; take \
screenshots to see the result. Good workflow: scene_components and scene_list to inspect, plan \
with coordinates in metres (Y up), build with scene_batch (\"@N\" refers to the id created by \
command N of the batch), then camera_frame + screenshot and iterate with scene_update. Engine \
errors name the offending field and what was expected — fix the call instead of repeating it. \
The build_world and inspect_scene prompts describe the workflow in detail.";

/// The quat-mcp server. Cheap to clone; all clones share one engine connection.
#[derive(Clone)]
pub struct QuatServer {
    engine: Arc<EngineClient>,
    long_timeout: Duration,
    tool_router: ToolRouter<Self>,
    prompt_router: PromptRouter<Self>,
}

impl std::fmt::Debug for QuatServer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("QuatServer")
            .field("engine", &self.engine.config())
            .field("long_timeout", &self.long_timeout)
            .finish_non_exhaustive()
    }
}

fn to_params<T: Serialize>(p: &T) -> Value {
    serde_json::to_value(p).unwrap_or_else(|_| json!({}))
}

/// A successful engine result as compact JSON text.
fn json_result(v: &Value) -> CallToolResult {
    CallToolResult::success(vec![ContentBlock::text(v.to_string())])
}

/// A failed call as a tool error the model can read and act on.
pub fn error_result(e: &EngineError) -> CallToolResult {
    let mut text = e.to_string();
    let hint = match e.rpc_code() {
        Some(code::INVALID_PARAMS) => Some(
            "Fix the field named above and call again; the tool description lists every \
             component and its fields.",
        ),
        Some(code::NOT_FOUND) => Some(
            "Use scene_list (filter with name_contains) to find valid entity ids or names, and \
             asset_list_models for model paths.",
        ),
        Some(code::UNAVAILABLE) => Some(
            "The running engine cannot do this in its current build or state (e.g. it runs \
             without a window or editor); continue without it.",
        ),
        Some(code::METHOD_NOT_FOUND) => Some(
            "The running engine does not implement this method (yet); engine_methods lists \
             what it supports.",
        ),
        _ => None,
    };
    if let Some(hint) = hint {
        text.push_str("\nHint: ");
        text.push_str(hint);
    }
    if let EngineError::Rpc {
        data: Some(data), ..
    } = e
    {
        text.push_str("\nDetails: ");
        text.push_str(&data.to_string());
    }
    CallToolResult::error(vec![ContentBlock::text(text)])
}

fn invalid_input(message: impl Into<String>) -> CallToolResult {
    CallToolResult::error(vec![ContentBlock::text(message.into())])
}

/// Map a tool-style method name ("scene_create") to the engine's ("scene.create").
pub fn normalize_method(method: &str) -> String {
    let m = method.trim();
    if m.contains('.') {
        return m.to_owned();
    }
    if m == "screenshot" {
        return "render.screenshot".to_owned();
    }
    match m.split_once('_') {
        Some((ns, rest)) => format!("{ns}.{rest}"),
        None => m.to_owned(),
    }
}

/// Prefix a summary when some batch commands failed or were skipped.
fn batch_summary(commands: usize, result: &Value) -> Option<String> {
    let results = result.get("results")?.as_array()?;
    let failed: Vec<(usize, &Value)> = results
        .iter()
        .enumerate()
        .filter(|(_, r)| r.get("ok") == Some(&Value::Bool(false)))
        .collect();
    let skipped = commands.saturating_sub(results.len());
    if failed.is_empty() && skipped == 0 {
        return None;
    }
    let mut s = format!(
        "{} of {} commands failed{}.",
        failed.len(),
        commands,
        if skipped > 0 {
            format!(", {skipped} not run (stop_on_error)")
        } else {
            String::new()
        }
    );
    for (i, r) in failed.iter().take(5) {
        let err = r.get("error");
        let code = err.and_then(|e| e.get("code")).and_then(Value::as_i64);
        let msg = err
            .and_then(|e| e.get("message"))
            .and_then(Value::as_str)
            .unwrap_or("unknown error");
        s.push_str(&format!(
            "\n- command {i}: {} ({}): {msg}",
            crate::client::code_name(code.unwrap_or(0)),
            code.unwrap_or(0)
        ));
    }
    if failed.len() > 5 {
        s.push_str(&format!("\n- … and {} more", failed.len() - 5));
    }
    Some(s)
}

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

#[tool_router(router = tool_router)]
impl QuatServer {
    pub fn new(engine: Arc<EngineClient>, long_timeout: Duration) -> Self {
        Self {
            engine,
            long_timeout,
            tool_router: Self::tool_router(),
            prompt_router: Self::prompt_router(),
        }
    }

    async fn run(&self, method: &str, params: Option<Value>) -> CallToolResult {
        match self.engine.call(method, params).await {
            Ok(v) => json_result(&v),
            Err(e) => error_result(&e),
        }
    }

    async fn run_long(&self, method: &str, params: Option<Value>) -> CallToolResult {
        let timeout = self.long_timeout.max(self.engine.config().timeout);
        match self.engine.call_with_timeout(method, params, timeout).await {
            Ok(v) => json_result(&v),
            Err(e) => error_result(&e),
        }
    }

    // ---- engine -----------------------------------------------------------

    #[tool(
        description = "Engine status: frame number, entity count, whether play mode is running \
and whether the editor is present. Also confirms quat-mcp can reach the engine.",
        annotations(read_only_hint = true)
    )]
    async fn engine_info(&self) -> CallToolResult {
        self.run("engine.info", None).await
    }

    #[tool(
        description = "List the remote-control methods the running engine implements \
[{name, summary, deferred}]. Use it when a tool reports method_not_found.",
        annotations(read_only_hint = true)
    )]
    async fn engine_methods(&self) -> CallToolResult {
        self.run("rpc.methods", None).await
    }

    #[tool(
        description = "Call any engine remote-control method by name with a params object, and \
return its raw result. Escape hatch for methods newer than this server; prefer the dedicated \
tools, which document their parameters."
    )]
    async fn engine_call(&self, params: Parameters<EngineCallParams>) -> CallToolResult {
        let p = params.0;
        let method = normalize_method(&p.method);
        if method == "render.screenshot" {
            return self
                .screenshot(Parameters(
                    p.params
                        .and_then(|m| serde_json::from_value(Value::Object(m)).ok())
                        .unwrap_or_default(),
                ))
                .await;
        }
        self.run_long(&method, p.params.map(Value::Object)).await
    }

    // ---- scene: read ------------------------------------------------------

    #[tool(
        description = "List the component types the engine understands: \
[{name, summary, addable, writable}]. addable=false components cannot be added by \
scene_create/scene_update (e.g. animator is edit-only, mesh is read-only). Call this first when \
building.",
        annotations(read_only_hint = true)
    )]
    async fn scene_components(&self) -> CallToolResult {
        self.run("scene.components", None).await
    }

    #[tool(
        description = "List entities: {total, entities:[{id, name, parent, components, \
world_position}]}. Filter by name substring or by component; page with limit (default 100) and \
offset. Use the returned ids to reference entities.",
        annotations(read_only_hint = true)
    )]
    async fn scene_list(&self, params: Parameters<SceneListParams>) -> CallToolResult {
        self.run("scene.list", Some(to_params(&params.0))).await
    }

    #[tool(
        description = "Get one entity in full: {id, name, parent, children, components:{...}} \
with every component's current field values (same field names as scene_update accepts).",
        annotations(read_only_hint = true)
    )]
    async fn scene_get(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.run("scene.get", Some(to_params(&params.0))).await
    }

    // ---- scene: write -----------------------------------------------------

    #[tool(description = concat!(
        "Create one entity with optional name, parent and components; returns {id, name}. \
Example: {\"name\":\"Crate\",\"components\":{\"transform\":{\"position\":[0,0.5,0]},\
\"primitive\":{\"shape\":\"cube\",\"color\":\"#8b5a2b\",\"roughness\":0.8},\
\"collider\":{\"shape\":\"box\",\"half_extents\":[0.5,0.5,0.5]},\
\"rigidbody\":{\"motion\":\"dynamic\",\"mass\":20}}}. To create many entities use scene_batch.",
        components_doc!()
    ))]
    async fn scene_create(&self, params: Parameters<SceneCreateParams>) -> CallToolResult {
        self.run("scene.create", Some(to_params(&params.0))).await
    }

    #[tool(
        description = concat!(
            "Rename an entity and/or add or merge-update its components; returns {id}. Only the \
fields you give change, e.g. {\"entity\":12,\"components\":{\"transform\":{\"rotation\":[0,45,0]}}} \
turns entity 12 to a 45° heading and keeps its position and scale. Adding a component the entity \
lacks creates it with defaults for omitted fields.",
            components_doc!()
        ),
        annotations(idempotent_hint = true)
    )]
    async fn scene_update(&self, params: Parameters<SceneUpdateParams>) -> CallToolResult {
        self.run("scene.update", Some(to_params(&params.0))).await
    }

    #[tool(
        description = "Remove components from an entity, e.g. [\"rigidbody\",\"collider\"]; \
returns {removed:[names]}. Removing primitive also removes its mesh. transform cannot be removed.",
        annotations(destructive_hint = true)
    )]
    async fn scene_remove_components(
        &self,
        params: Parameters<RemoveComponentsParams>,
    ) -> CallToolResult {
        self.run("scene.remove_components", Some(to_params(&params.0)))
            .await
    }

    #[tool(
        description = "Destroy an entity and all its descendants; returns {destroyed: n}.",
        annotations(destructive_hint = true)
    )]
    async fn scene_destroy(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.run("scene.destroy", Some(to_params(&params.0))).await
    }

    #[tool(
        description = "Re-parent an entity (parent null = make it a root entity); returns {id}. \
keep_world (default true) keeps it where it is in the world; false keeps its local transform so \
it jumps to the same offset relative to the new parent.",
        annotations(idempotent_hint = true)
    )]
    async fn scene_set_parent(&self, params: Parameters<SetParentParams>) -> CallToolResult {
        self.run("scene.set_parent", Some(to_params(&params.0)))
            .await
    }

    #[tool(
        description = "Delete every entity in the scene (lights are kept unless \
keep_lights=false); returns {destroyed: n}. Use to start a world from scratch.",
        annotations(destructive_hint = true)
    )]
    async fn scene_clear(&self, params: Parameters<SceneClearParams>) -> CallToolResult {
        self.run_long("scene.clear", Some(to_params(&params.0)))
            .await
    }

    #[tool(description = concat!(
        "Run many commands in one engine frame — the efficient way to build. Each command is \
{method, params} where method is an engine method (scene.create, scene.update, \
scene.remove_components, scene.destroy, scene.set_parent, scene.clear, environment.set, \
camera.set, camera.frame, world.ground_height, …) and params is exactly what the matching tool \
takes. In entity/parent fields, the string \"@N\" means the id returned by command N (0-based) of \
this batch, so you can create a parent and its children together:\n\
[{\"method\":\"scene.create\",\"params\":{\"name\":\"Table\",\"components\":{\"transform\":\
{\"position\":[0,0,0]}}}},\n\
 {\"method\":\"scene.create\",\"params\":{\"name\":\"Table_Top\",\"parent\":\"@0\",\
\"components\":{\"transform\":{\"position\":[0,0.75,0],\"scale\":[1.6,0.06,0.9]},\
\"primitive\":{\"shape\":\"cube\",\"color\":\"#8b5a2b\"}}}}]\n\
Returns {results:[{ok:true,result}|{ok:false,error:{code,message}}]} in command order; with \
stop_on_error (default true) the batch stops at the first failure and later commands do not run \
(earlier ones stay applied). Screenshots cannot be batched. Keep batches to a few hundred \
commands.",
        components_doc!()
    ))]
    async fn scene_batch(&self, params: Parameters<SceneBatchParams>) -> CallToolResult {
        let mut p = params.0;
        if p.commands.is_empty() {
            return invalid_input("scene_batch: `commands` is empty; give at least one command.");
        }
        for c in &mut p.commands {
            c.method = normalize_method(&c.method);
        }
        let commands = p.commands.len();
        let timeout = self.long_timeout.max(self.engine.config().timeout);
        match self
            .engine
            .call_with_timeout("scene.batch", Some(to_params(&p)), timeout)
            .await
        {
            Ok(v) => {
                let mut content = Vec::with_capacity(2);
                if let Some(summary) = batch_summary(commands, &v) {
                    content.push(ContentBlock::text(summary));
                }
                content.push(ContentBlock::text(v.to_string()));
                CallToolResult::success(content)
            }
            Err(e) => error_result(&e),
        }
    }

    // ---- environment / world ----------------------------------------------

    #[tool(
        description = "Current sky: {sun:{azimuth_deg, elevation_deg, intensity, color}, \
clouds:{enabled, coverage, density, bottom_km, thickness_km, size_km, detail, wind_speed, \
wind_dir_deg}}. Sun angles: azimuth_deg = atan2(d.z, d.x) and elevation_deg = asin(-d.y) of the \
light's travel direction d, so elevation 90 = noon overhead, 5..10 = golden hour, < 0 = night.",
        annotations(read_only_hint = true)
    )]
    async fn environment_get(&self) -> CallToolResult {
        self.run("environment.get", None).await
    }

    #[tool(
        description = "Change the sun and/or clouds; every field is optional and omitted fields \
keep their values. Returns the new environment (as environment_get). Examples: sunset \
{\"sun\":{\"elevation_deg\":6,\"azimuth_deg\":200}}; overcast \
{\"clouds\":{\"enabled\":true,\"coverage\":0.85,\"density\":40}}; clear sky \
{\"clouds\":{\"enabled\":false}}.",
        annotations(idempotent_hint = true)
    )]
    async fn environment_set(&self, params: Parameters<EnvironmentSetParams>) -> CallToolResult {
        self.run("environment.set", Some(to_params(&params.0)))
            .await
    }

    #[tool(
        description = "Terrain height (world y, metres) under world-space [x, z] points: \
{heights:[y | null]} in input order, null where no terrain covers the point. Use it to place \
objects on uneven ground (set position y = height + half the object's height). At most 1024 \
points per call.",
        annotations(read_only_hint = true)
    )]
    async fn world_ground_height(&self, params: Parameters<GroundHeightParams>) -> CallToolResult {
        let n = params.0.points.len();
        if n == 0 {
            return invalid_input("world_ground_height: `points` is empty; give [[x, z], ...].");
        }
        if n > MAX_GROUND_POINTS {
            return invalid_input(format!(
                "world_ground_height: {n} points given, at most {MAX_GROUND_POINTS} per call; \
                 split the query."
            ));
        }
        self.run("world.ground_height", Some(to_params(&params.0)))
            .await
    }

    // ---- camera / screenshot ----------------------------------------------

    #[tool(
        description = "Current viewport camera: {position, yaw_deg, pitch_deg, fov_deg}. yaw 0 \
looks along +X and 90 along +Z; positive pitch looks up.",
        annotations(read_only_hint = true)
    )]
    async fn camera_get(&self) -> CallToolResult {
        self.run("camera.get", None).await
    }

    #[tool(
        description = "Move the viewport camera; returns the new camera (as camera_get). Usually \
give position plus look_at, e.g. {\"position\":[12,8,12],\"look_at\":[0,1,0]}. Alternatively \
set yaw_deg (0 = +X, 90 = +Z) and pitch_deg (positive = up). fov_deg is the vertical field of \
view.",
        annotations(idempotent_hint = true)
    )]
    async fn camera_set(&self, params: Parameters<CameraSetParams>) -> CallToolResult {
        self.run("camera.set", Some(to_params(&params.0))).await
    }

    #[tool(
        description = "Point the viewport camera so an entity (or, with no entity, the whole \
scene) fills the view; returns the new camera. Handy right before a screenshot.",
        annotations(idempotent_hint = true)
    )]
    async fn camera_frame(&self, params: Parameters<CameraFrameParams>) -> CallToolResult {
        self.run("camera.frame", Some(to_params(&params.0))).await
    }

    #[tool(
        description = "Capture the engine viewport as a PNG image so you can see the world. \
Returns the image plus its size. Position the camera first (camera_frame or camera_set). Use \
after building to check proportions, placement (floating or buried objects), colours and \
lighting, then fix and look again.",
        annotations(read_only_hint = true)
    )]
    async fn screenshot(&self, params: Parameters<ScreenshotParams>) -> CallToolResult {
        let timeout = self.long_timeout.max(self.engine.config().timeout);
        let v = match self
            .engine
            .call_with_timeout("render.screenshot", Some(to_params(&params.0)), timeout)
            .await
        {
            Ok(v) => v,
            Err(e) => return error_result(&e),
        };
        let Some(data) = v.get("data").and_then(Value::as_str) else {
            return invalid_input(
                "render.screenshot: the engine reply has no `data` field with the image; this is \
                 an engine bug.",
            );
        };
        let mime = v.get("mime").and_then(Value::as_str).unwrap_or("image/png");
        let width = v.get("width").and_then(Value::as_u64).unwrap_or(0);
        let height = v.get("height").and_then(Value::as_u64).unwrap_or(0);
        CallToolResult::success(vec![
            ContentBlock::image(data, mime),
            ContentBlock::text(format!(
                "Viewport screenshot, {width}x{height} px ({mime})."
            )),
        ])
    }

    // ---- assets -----------------------------------------------------------

    #[tool(
        description = "List glTF models available to spawn: {models:[project-relative paths of \
.gltf/.glb files]}.",
        annotations(read_only_hint = true)
    )]
    async fn asset_list_models(&self) -> CallToolResult {
        self.run("asset.list_models", None).await
    }

    #[tool(description = concat!(
        "Spawn a glTF model (path from asset_list_models) into the scene, optionally named, \
parented and with extra components on its root (e.g. a transform to place and scale it). \
Returns {roots:[ids]} — the model's top-level entities. Animated models get an animator \
component.",
        components_doc!()
    ))]
    async fn asset_spawn_model(&self, params: Parameters<SpawnModelParams>) -> CallToolResult {
        self.run_long("asset.spawn_model", Some(to_params(&params.0)))
            .await
    }

    // ---- simulation / files / navigation ----------------------------------

    #[tool(
        description = "Enter play mode: physics, animation, NPC behaviour and navigation start \
running. The editor restores the scene when play stops. Returns {playing}."
    )]
    async fn sim_play(&self) -> CallToolResult {
        self.run("sim.play", None).await
    }

    #[tool(
        description = "Leave play mode; the scene returns to its state before sim_play. \
Returns {playing}."
    )]
    async fn sim_stop(&self) -> CallToolResult {
        self.run("sim.stop", None).await
    }

    #[tool(
        description = "Save the scene as JSON to a project-relative path (e.g. \
\"assets/scenes/village.json\"); returns {path}. Overwrites an existing file."
    )]
    async fn scene_save(&self, params: Parameters<ScenePathParams>) -> CallToolResult {
        self.run_long("scene.save", Some(to_params(&params.0)))
            .await
    }

    #[tool(
        description = "Replace the current scene with one loaded from a project-relative JSON \
path; returns {path}. Unsaved changes are lost.",
        annotations(destructive_hint = true)
    )]
    async fn scene_load(&self, params: Parameters<ScenePathParams>) -> CallToolResult {
        self.run_long("scene.load", Some(to_params(&params.0)))
            .await
    }

    #[tool(
        description = "Bake the navigation mesh from the current static geometry and terrain \
(needed before nav_agent / patrol entities can walk); returns {polygons}. Re-bake after moving \
static geometry. Can take a few seconds on large terrain."
    )]
    async fn nav_bake(&self) -> CallToolResult {
        self.run_long("nav.bake", None).await
    }
}

#[prompt_router]
impl QuatServer {
    /// Step-by-step workflow for building a world from a description.
    #[prompt(
        name = "build_world",
        description = "Build a 3D world in the running quat-3d engine from a description: inspect, \
plan, build with batches, screenshot, iterate."
    )]
    async fn build_world(&self, args: Parameters<BuildWorldArgs>) -> Vec<PromptMessage> {
        let a = args.0;
        let constraints = a
            .constraints
            .filter(|c| !c.trim().is_empty())
            .map(|c| format!("\nConstraints: {c}\n"))
            .unwrap_or_default();
        vec![PromptMessage::new_text(
            Role::User,
            format!(
                "Build this in the running quat-3d engine using the quat-mcp tools: {}\n{constraints}
Work in this order:

1. Inspect. Call scene_components (which components exist and can be added), scene_list \
(what is already there), environment_get and camera_get. If the scene has terrain, query \
world_ground_height for the spots you will build on.

2. Plan before creating anything. Write a short plan: the layout with coordinates in metres \
(Y is up; flat ground is y = 0 unless there is terrain), and for each object its name, shape, \
position, rotation, scale and colour; the lights; the time of day and clouds. Give entities \
unique descriptive names and group parts under a parent (e.g. \"House_1\" with children \
\"House_1_Walls\", \"House_1_Roof\") so they can be moved together.

3. Build in few round trips. Use scene_batch with many scene.create commands; refer to an \
entity created earlier in the same batch with \"@N\" (its 0-based command index) in parent or \
entity fields. Primitives are about unit size and centred on their position: size them with \
transform.scale, and set position y to half the height so they rest on the ground. Add \
collider + rigidbody only where physics matters.

4. Set the mood. environment_set for the sun (elevation_deg 5..10 for golden hour, 45..70 for \
daylight) and clouds; point_light entities for lamps, fires and windows (warm linear colours \
such as [1.0, 0.6, 0.3]).

5. Look. camera_frame (whole scene or one entity) or camera_set with position + look_at, then \
screenshot. Compare the image with the goal: proportions, gaps, objects floating or buried, \
overlaps, colours, lighting, composition. Take a second screenshot from another angle for \
anything large.

6. Iterate. Fix problems with scene_update (it merges: give only the fields that change) or \
another scene_batch, then screenshot again. Repeat until the result matches the description.

7. Finish with a short summary of what you built; save with scene_save if asked.

Conventions: rotation is Euler degrees [x pitch, y yaw, z roll] applied in YXZ order; scale is \
a number or [x, y, z]; primitive colours are \"#rrggbb\" sRGB hex; light colours are linear \
[r, g, b]. Engine errors name the bad field and what was expected — read them and correct the \
call instead of repeating it.",
                a.description.trim()
            ),
        )]
    }

    /// Step-by-step workflow for describing and reviewing the current scene.
    #[prompt(
        name = "inspect_scene",
        description = "Inspect the running quat-3d scene and report its structure, lighting, \
physics and any problems."
    )]
    async fn inspect_scene(&self, args: Parameters<InspectSceneArgs>) -> Vec<PromptMessage> {
        let focus = args
            .0
            .focus
            .filter(|f| !f.trim().is_empty())
            .map(|f| format!("\nFocus especially on: {f}\n"))
            .unwrap_or_default();
        vec![PromptMessage::new_text(
            Role::User,
            format!(
                "Inspect the scene in the running quat-3d engine using the quat-mcp tools and \
report what you find.{focus}

1. engine_info for the overall state (entity count, play mode).
2. scene_list, paging with offset until you have seen `total` entities; note the hierarchy \
(parent ids), names and which components each entity has.
3. scene_get on the entities that matter (lights, cameras, terrain, physics bodies, NPCs, and \
anything related to the focus) to read their component values.
4. environment_get and camera_get.
5. camera_frame with no entity, then screenshot; frame and screenshot specific areas if useful.

Then report: a short description of the scene, its hierarchy, lighting (sun + lights), physics \
and gameplay components, and problems you notice — unnamed or duplicate-named entities, objects \
floating above or sunk into the ground, overlapping geometry, dynamic bodies without colliders, \
nav agents without a baked navmesh, scenes too dark or blown out. Do not change anything unless \
asked."
            ),
        )]
    }
}

impl QuatServer {
    async fn scene_summary(&self) -> Result<Value, EngineError> {
        let info = self.engine.call("engine.info", None).await?;
        let list = self
            .engine
            .call("scene.list", Some(json!({"limit": 500})))
            .await?;
        // The sky may be unavailable (headless engine); the summary still works.
        let environment = self.engine.call("environment.get", None).await.ok();
        let camera = self.engine.call("camera.get", None).await.ok();
        Ok(json!({
            "engine": info,
            "scene": list,
            "environment": environment,
            "camera": camera,
        }))
    }
}

#[tool_handler(router = self.tool_router)]
#[prompt_handler(router = self.prompt_router)]
impl ServerHandler for QuatServer {
    fn get_info(&self) -> ServerConfig {
        ServerConfig::new(
            ServerCapabilities::builder()
                .enable_tools()
                .enable_prompts()
                .enable_resources()
                .build(),
        )
        .with_server_info(Implementation::new("quat-mcp", env!("CARGO_PKG_VERSION")))
        .with_instructions(INSTRUCTIONS)
    }

    async fn list_resources(
        &self,
        _request: Option<PaginatedRequestParams>,
        _context: RequestContext<RoleServer>,
    ) -> Result<ListResourcesResult, ErrorData> {
        Ok(ListResourcesResult::with_all_items(vec![
            Resource::new(SCENE_SUMMARY_URI, "scene_summary")
                .with_title("Current scene summary")
                .with_description(
                    "Live snapshot of the running engine: engine.info, the first 500 entities \
                     (scene.list), the environment and the camera, as JSON.",
                )
                .with_mime_type("application/json"),
        ]))
    }

    async fn read_resource(
        &self,
        request: ReadResourceRequestParams,
        _context: RequestContext<RoleServer>,
    ) -> Result<ReadResourceResponse, ErrorData> {
        if request.uri != SCENE_SUMMARY_URI {
            return Err(ErrorData::resource_not_found(
                format!(
                    "unknown resource `{}`; available: {SCENE_SUMMARY_URI}",
                    request.uri
                ),
                None,
            ));
        }
        match self.scene_summary().await {
            Ok(summary) => {
                let text =
                    serde_json::to_string_pretty(&summary).unwrap_or_else(|_| summary.to_string());
                Ok(ReadResourceResult::new(vec![
                    ResourceContents::text(text, SCENE_SUMMARY_URI)
                        .with_mime_type("application/json"),
                ])
                .into())
            }
            Err(e) => Err(ErrorData::internal_error(e.to_string(), None)),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn method_names_are_normalized() {
        assert_eq!(normalize_method("scene.create"), "scene.create");
        assert_eq!(normalize_method("scene_create"), "scene.create");
        assert_eq!(
            normalize_method("scene_remove_components"),
            "scene.remove_components"
        );
        assert_eq!(
            normalize_method("world_ground_height"),
            "world.ground_height"
        );
        assert_eq!(normalize_method(" screenshot "), "render.screenshot");
    }

    #[test]
    fn batch_summary_reports_failures_and_skips() {
        let ok = json!({"results": [{"ok": true, "result": {"id": 1}}]});
        assert!(batch_summary(1, &ok).is_none());
        let bad = json!({"results": [
            {"ok": true, "result": {"id": 1}},
            {"ok": false, "error": {"code": -32602, "message": "primitive.shape: unknown"}}
        ]});
        let s = batch_summary(3, &bad).unwrap();
        assert!(s.starts_with("1 of 3 commands failed, 1 not run"));
        assert!(s.contains("command 1: invalid_params (-32602): primitive.shape: unknown"));
    }

    #[test]
    fn every_tool_has_an_object_schema_and_description() {
        let tools = QuatServer::tool_router().list_all();
        assert!(tools.len() >= 25, "{} tools", tools.len());
        for t in &tools {
            assert!(
                t.description.as_deref().is_some_and(|d| d.len() > 30),
                "{} lacks a description",
                t.name
            );
            assert_eq!(
                t.input_schema.get("type"),
                Some(&Value::String("object".into())),
                "{}",
                t.name
            );
        }
    }
}
