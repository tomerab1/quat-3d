//! Typed tool inputs. The doc comments become the JSON-schema descriptions
//! the model reads, so they state units and conventions explicitly.

use schemars::JsonSchema;
use serde::de::Error as _;
use serde::{Deserialize, Deserializer, Serialize};
use serde_json::{Map, Value};

mod behavior;
mod debug;
mod project;
mod script;
mod shader;
mod tuning;
mod world;
pub use behavior::*;
pub use debug::*;
pub use project::*;
pub use script::*;
pub use shader::*;
pub use tuning::*;
pub use world::*;

/// A JSON object keyed by component name (see the tool description).
pub type Components = Map<String, Value>;

/// An entity: its integer id (preferred), or its exact name (must match exactly
/// one entity). Inside `scene_batch`, the string "@N" means the id created by
/// command N (0-based) of the same batch.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum EntityRef {
    /// Entity id, as returned by `scene_create` / `scene_list`.
    Id(u64),
    /// Exact entity name, or "@N" (batch back-reference).
    Name(String),
}

/// Accepts a JSON object, or a string holding one (some clients stringify
/// nested objects). `null` and absent both mean "not given".
fn object_or_json_string<'de, D>(d: D) -> Result<Option<Components>, D::Error>
where
    D: Deserializer<'de>,
{
    match Option::<Value>::deserialize(d)? {
        None | Some(Value::Null) => Ok(None),
        Some(Value::Object(m)) => Ok(Some(m)),
        Some(Value::String(s)) => match serde_json::from_str::<Value>(&s) {
            Ok(Value::Object(m)) => Ok(Some(m)),
            _ => Err(D::Error::custom(
                "expected a JSON object (got a string that does not contain a JSON object)",
            )),
        },
        Some(other) => Err(D::Error::custom(format!(
            "expected a JSON object, got {}",
            json_type(&other)
        ))),
    }
}

fn json_type(v: &Value) -> &'static str {
    match v {
        Value::Null => "null",
        Value::Bool(_) => "a boolean",
        Value::Number(_) => "a number",
        Value::String(_) => "a string",
        Value::Array(_) => "an array",
        Value::Object(_) => "an object",
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SceneListParams {
    /// Only entities whose name contains this substring (case-sensitive).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name_contains: Option<String>,
    /// Only entities that have this component (e.g. "`point_light`", "primitive").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub component: Option<String>,
    /// Page size (default 100).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub limit: Option<u32>,
    /// Number of matching entities to skip (default 0), for paging through `total`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub offset: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EntityParams {
    /// The entity: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SceneCreateParams {
    /// Display name (shown in the editor hierarchy; usable as a reference later,
    /// so prefer unique, descriptive names like "`Tower_Left`").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Parent entity (id or unique name). Omit or null for a root entity. The
    /// child's transform is then relative to the parent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent: Option<EntityRef>,
    /// Components to add, keyed by component name, e.g.
    /// {"transform":{"position":[0,1,0]},"primitive":{"shape":"cube","color":"#c0392b"}}.
    /// Omitted fields take their defaults.
    #[serde(
        default,
        deserialize_with = "object_or_json_string",
        skip_serializing_if = "Option::is_none"
    )]
    pub components: Option<Components>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SceneUpdateParams {
    /// The entity to change: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// New name.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Components to add or merge-update, keyed by component name. Only the
    /// fields you give change; e.g. {"transform":{"position":[2,0,0]}} moves
    /// the entity and keeps its rotation and scale.
    #[serde(
        default,
        deserialize_with = "object_or_json_string",
        skip_serializing_if = "Option::is_none"
    )]
    pub components: Option<Components>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RemoveComponentsParams {
    /// The entity: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// Component names to remove, e.g. `["rigidbody", "collider"]`. `transform`
    /// cannot be removed.
    pub components: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SetParentParams {
    /// The entity to re-parent: integer id (preferred) or exact unique name.
    pub entity: EntityRef,
    /// New parent (id or unique name); null (or omitted) makes it a root entity.
    #[serde(default)]
    pub parent: Option<EntityRef>,
    /// Keep the entity where it is in the world (default true). false keeps
    /// its local transform instead, so it moves with the new parent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub keep_world: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SceneClearParams {
    /// Keep light entities (default true) so the cleared scene is not black.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub keep_lights: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BatchCommand {
    /// Engine method name, e.g. "scene.create", "scene.update",
    /// "`scene.set_parent`", "environment.set", "camera.set". (Tool-style names
    /// such as "`scene_create`" are converted automatically.)
    pub method: String,
    /// That method's parameters, exactly as for the matching tool. In `entity`
    /// and `parent` fields, "@N" refers to the id returned by command N
    /// (0-based) of this batch.
    #[serde(
        default,
        deserialize_with = "object_or_json_string",
        skip_serializing_if = "Option::is_none"
    )]
    pub params: Option<Components>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SceneBatchParams {
    /// Commands executed in order, in a single engine frame.
    pub commands: Vec<BatchCommand>,
    /// Stop at the first failing command (default true). With false every
    /// command is attempted and each result reports its own success.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub stop_on_error: Option<bool>,
}

/// A colour: "#rrggbb" (sRGB hex) or a linear [r, g, b] array.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum Color {
    /// sRGB hex, "#rrggbb".
    Hex(String),
    /// Linear RGB components, 0..1 (may exceed 1 for HDR).
    Rgb(Vec<f64>),
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SunParams {
    /// Compass direction of the sun's travel in degrees: atan2(d.z, d.x) of
    /// the light's travel direction d (the same angle the editor shows).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub azimuth_deg: Option<f64>,
    /// Sun height above the horizon in degrees: 90 = noon overhead, ~5 = sunset,
    /// negative = below the horizon (night).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub elevation_deg: Option<f64>,
    /// Sun light intensity (same units as `dir_light.intensity`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub intensity: Option<f64>,
    /// Sun colour, in the format `environment_get` returns (linear [r,g,b]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub color: Option<Color>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CloudParams {
    /// Turn the volumetric cloud layer on or off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Fraction of the sky covered, 0..1 (default 0.42).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub coverage: Option<f64>,
    /// Extinction per km at full density, 0..128 (default 24); higher = darker, more opaque clouds.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub density: Option<f64>,
    /// Altitude of the cloud layer's base above the ground, km, 0.2..15 (default 1.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub bottom_km: Option<f64>,
    /// Thickness of the cloud layer, km, 0.2..10 (default 2.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub thickness_km: Option<f64>,
    /// Base size of cloud features, km, 1..80 (default 10).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub size_km: Option<f64>,
    /// Edge erosion / wispiness, 0..1 (default 0.4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<f64>,
    /// Drift speed, m/s, 0..100 (default 12).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_speed: Option<f64>,
    /// World-space heading the clouds drift toward, degrees, -180..180 (default 35).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_dir_deg: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EnvironmentSetParams {
    /// Sun position and light. Omitted fields keep their values.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sun: Option<SunParams>,
    /// Volumetric clouds. Omitted fields keep their values.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub clouds: Option<CloudParams>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GroundHeightParams {
    /// World-space [x, z] points in metres (at most 1024).
    pub points: Vec<[f64; 2]>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CameraSetParams {
    /// Camera position [x, y, z] in metres (Y is up).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub position: Option<[f64; 3]>,
    /// World point [x, y, z] to aim at (overrides yaw/pitch).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub look_at: Option<[f64; 3]>,
    /// Heading in degrees: 0 looks along +X, 90 along +Z.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub yaw_deg: Option<f64>,
    /// Elevation in degrees: positive looks up, negative looks down (-89..89).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub pitch_deg: Option<f64>,
    /// Vertical field of view in degrees.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fov_deg: Option<f64>,
    /// Near clip plane in metres, or "auto" (derived from the scene). Omit to
    /// keep the current setting.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub near: Option<ClipPlane>,
    /// Far clip plane in metres, or "auto" (derived from the scene: reaches
    /// the farthest object and the terrain). Omit to keep the current setting.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub far: Option<ClipPlane>,
    /// true: this move is a cut (teleport) and temporal anti-aliasing history
    /// restarts; false: continuous motion (e.g. a fly-through made of many
    /// small `camera_set` calls). Default: a cut when the camera jumps more
    /// than 10% of the far plane or turns more than 30 degrees.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cut: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CameraFrameParams {
    /// Entity to frame (id or unique name). Omit to frame the whole scene.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScreenshotParams {
    /// Longest side of the returned image in pixels (default 1024). Smaller
    /// images are cheaper to look at; use 512 for quick checks.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_size: Option<u32>,
    /// Render from this camera instead of the viewport camera, without moving
    /// the user's view. Omitted fields keep the viewport camera's values.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub camera: Option<ShotCamera>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SpawnModelParams {
    /// Project-relative path of a .gltf/.glb file, exactly as `asset_list_models`
    /// returns it.
    pub path: String,
    /// Name for the spawned root entity.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Parent entity (id or unique name); omit for root.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub parent: Option<EntityRef>,
    /// Extra components for the spawned root, e.g. {"transform":{"position":[0,0,5],"scale":2}}.
    #[serde(
        default,
        deserialize_with = "object_or_json_string",
        skip_serializing_if = "Option::is_none"
    )]
    pub components: Option<Components>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ScenePathParams {
    /// Project-relative path, e.g. "assets/scenes/castle.json". Paths that
    /// escape the project ("..", absolute) are refused.
    pub path: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EngineCallParams {
    /// Engine method name, e.g. "engine.info" (see `engine_methods`).
    pub method: String,
    /// Parameters object for the method; omit for none.
    #[serde(
        default,
        deserialize_with = "object_or_json_string",
        skip_serializing_if = "Option::is_none"
    )]
    pub params: Option<Components>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn entity_ref_accepts_ids_and_names() {
        let p: EntityParams = serde_json::from_value(json!({"entity": 42})).unwrap();
        assert_eq!(p.entity, EntityRef::Id(42));
        let p: EntityParams = serde_json::from_value(json!({"entity": "@3"})).unwrap();
        assert_eq!(p.entity, EntityRef::Name("@3".into()));
    }

    #[test]
    fn components_accept_object_or_stringified_object() {
        let a: SceneCreateParams =
            serde_json::from_value(json!({"components": {"transform": {"position": [1, 2, 3]}}}))
                .unwrap();
        let b: SceneCreateParams =
            serde_json::from_value(json!({"components": "{\"transform\":{\"position\":[1,2,3]}}"}))
                .unwrap();
        assert_eq!(a.components, b.components);
        let none: SceneCreateParams = serde_json::from_value(json!({"components": null})).unwrap();
        assert!(none.components.is_none());
        let err = serde_json::from_value::<SceneCreateParams>(json!({"components": [1]}));
        assert!(
            err.unwrap_err()
                .to_string()
                .contains("expected a JSON object")
        );
    }

    #[test]
    fn unknown_fields_are_rejected() {
        let err = serde_json::from_value::<EnvironmentSetParams>(json!({"sun": {"elevation": 10}}))
            .unwrap_err();
        assert!(err.to_string().contains("elevation"));
    }

    #[test]
    fn set_parent_serializes_null_parent() {
        let p: SetParentParams = serde_json::from_value(json!({"entity": 1})).unwrap();
        let v = serde_json::to_value(&p).unwrap();
        assert_eq!(v, json!({"entity": 1, "parent": null}));
    }
}
