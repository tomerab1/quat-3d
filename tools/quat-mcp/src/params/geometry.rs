//! Inputs of the runtime geometry tools (`geometry_*`, `mesh_dynamic_*`;
//! docs/remote-control.md "Runtime geometry", docs/geometry.md).

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// A 2D point `[x, y]` in metres (plan coordinates: world x and z).
pub type Point2 = [f64; 2];
/// A 3D point `[x, y, z]` in metres.
pub type Point3 = [f64; 3];

/// A plan polygon: an outer ring and optional holes (rings are implicitly
/// closed; outer counter-clockwise, holes clockwise).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PolygonParam {
    /// Outer ring, at least 3 points.
    pub outer: Vec<Point2>,
    /// Holes inside the outer ring.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub holes: Vec<Vec<Point2>>,
}

/// Straight-skeleton roof over a footprint.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct RoofSpec {
    /// Footprint outer ring, counter-clockwise in plan (x, z).
    pub polygon: Vec<Point2>,
    /// Courtyards: clockwise rings inside the footprint.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub holes: Vec<Vec<Point2>>,
    /// One per edge (outer ring first, then holes): 1 = 45 degree slope, 0 = vertical gable, cot(pitch) in general.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub weights: Vec<f64>,
    /// Height of the eaves (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base_height: Option<f64>,
    /// Roof height per unit of inward distance at weight 1 (1 = 45 degrees), 0.001..1000.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub height_scale: Option<f64>,
    /// > 0: cut the roof flat at this height above the eaves.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub max_height: Option<f64>,
    /// Metres per texture repeat.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub uv_scale: Option<f64>,
}

/// A 2D profile swept along a path (walls, rails, kerbs).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SweepSpec {
    /// Centre line, at least 2 points; sample curves densely.
    pub path: Vec<Point3>,
    /// Cross-section: x sideways, y up (counter-clockwise when closed).
    pub profile: Vec<Point2>,
    /// The path loops (a ring wall).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub closed_path: Option<bool>,
    /// The profile is a closed outline (a solid wall) rather than a strip.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub closed_profile: Option<bool>,
    /// Cap the ends of a closed profile on an open path (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub caps: Option<bool>,
    /// Refuse corners that stretch the profile more than this (default 4).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub miter_limit: Option<f64>,
    /// Metres per texture repeat.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub uv_scale: Option<f64>,
    /// Profile corners sharper than this (degrees) are hard edges (default 40).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub crease_angle: Option<f64>,
}

/// A footprint extruded upward (walls of a building, plinths, towers).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ExtrudeSpec {
    /// Footprint outer ring, counter-clockwise in plan (x, z).
    pub polygon: Vec<Point2>,
    /// Holes (clockwise rings).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub holes: Vec<Vec<Point2>>,
    /// Height (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub height: Option<f64>,
    /// Bottom height (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub base_height: Option<f64>,
    /// Inset of the top outline (m; negative flares out).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub taper: Option<f64>,
    /// Close top and bottom (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub caps: Option<bool>,
    /// Metres per texture repeat.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub uv_scale: Option<f64>,
}

/// A seeded procedural stone or rock.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct StoneSpec {
    /// Seed: the same seed gives the same stone.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub seed: Option<u64>,
    /// Full extents `[x, y, z]` in metres.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub size: Option<[f32; 3]>,
    /// Detail level 0..5.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub subdivisions: Option<u32>,
    /// 0 = box-like, 1 = round.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub roundness: Option<f32>,
    /// Surface noise relative to the smallest extent, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub noise: Option<f32>,
    /// Flat chipped facets, 0..16.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chips: Option<u32>,
    /// Depth of the chips relative to the size, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub chip_depth: Option<f32>,
    /// Metres per texture repeat.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub uv_scale: Option<f32>,
}

/// A generator spec: what `geometry_generate` builds and what a
/// `dynamic_mesh` component's `generator` field holds.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum GeometrySpec {
    /// Straight-skeleton roof.
    Roof(RoofSpec),
    /// Profile swept along a path.
    Sweep(SweepSpec),
    /// Extruded footprint.
    Extrude(ExtrudeSpec),
    /// Seeded stone.
    Stone(StoneSpec),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GeometryGenerateParams {
    /// The generator and its inputs.
    pub spec: GeometrySpec,
    /// Store the result under this id (1-128 printable characters, no spaces); omit to only measure it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<String>,
}

/// A 2D polygon operation.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum PolygonOp {
    /// Grow (delta > 0) or shrink (delta < 0) the subject.
    Offset,
    /// subject OR clip.
    Union,
    /// subject AND clip.
    Intersect,
    /// subject minus clip.
    Subtract,
    /// Either but not both.
    Xor,
    /// Fix orientation, self-intersections and overlaps of the subject's rings.
    Repair,
    /// Drop near-collinear points (within `epsilon`).
    Simplify,
}

/// Corner style of an offset.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum JoinType {
    /// Sharp corners (up to `miter_limit`).
    Miter,
    /// Rounded corners.
    Round,
    /// Cut-off corners.
    Square,
}

/// How overlapping rings combine in `repair`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FillRule {
    /// Inside where the winding number is non-zero.
    NonZero,
    /// Inside where an odd number of rings overlap (nested rings make holes).
    EvenOdd,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GeometryPolygonParams {
    /// The operation.
    pub op: PolygonOp,
    /// Input polygons.
    pub subject: Vec<PolygonParam>,
    /// Second operand of union / intersect / subtract / xor.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub clip: Vec<PolygonParam>,
    /// Offset distance (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delta: Option<f64>,
    /// Offset corner style.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub join: Option<JoinType>,
    /// Miter corners longer than this (times |delta|) are squared off.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub miter_limit: Option<f64>,
    /// Max chord error of round corners (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub arc_tolerance: Option<f64>,
    /// Simplify tolerance (m).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub epsilon: Option<f64>,
    /// Fill rule for `repair`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fill: Option<FillRule>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DynamicMeshIdParams {
    /// Mesh id in the dynamic mesh store.
    pub id: String,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DynamicMeshSetParams {
    /// Mesh id (1-128 printable characters, no spaces).
    pub id: String,
    /// Vertex positions `[x, y, z]` (mesh-local metres).
    pub positions: Vec<[f32; 3]>,
    /// Triangle list: three vertex indices per triangle, counter-clockwise from the front.
    pub indices: Vec<u32>,
    /// Per-vertex normals; computed when omitted.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub normals: Vec<[f32; 3]>,
    /// Per-vertex texture coordinates, u and v interleaved (2 per vertex); box-projected when omitted.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub uvs: Vec<f32>,
    /// Computed normals: angle (degrees) above which edges stay hard (default 40).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub crease_angle: Option<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DynamicMeshUpdateParams {
    /// Mesh id.
    pub id: String,
    /// Index of the first vertex replaced.
    pub first: u32,
    /// New positions of vertices `first..first + len`.
    pub positions: Vec<[f32; 3]>,
    /// New normals for the same vertices (kept when omitted).
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub normals: Vec<[f32; 3]>,
}
