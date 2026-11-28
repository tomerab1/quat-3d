//! Inputs of the spline tools (`spline_*`, ROADMAP G18); docs/remote-control.md
//! has the engine methods behind them and docs/splines.md the concepts.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

use super::EntityRef;

/// How the control points make a curve.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum SplineKind {
    /// Smooth curve through every point (the default; no loops or cusps).
    CentripetalCatmullRom,
    /// Anchor, handle, handle, anchor, ... (3k + 1 points; 3k when closed).
    CubicBezier,
    /// Straight segments between the points.
    Polyline,
}

/// One control point: [x, y, z] in metres (entity-local), or an object with
/// per-point attributes.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum SplinePoint {
    /// [x, y, z] with width 1, height 0, tags 0.
    Position([f64; 3]),
    /// A point with attributes.
    Full(SplinePointFull),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplinePointFull {
    /// [x, y, z] in metres (entity-local).
    pub position: [f64; 3],
    /// Width across the curve in metres (0..10000, default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub width: Option<f64>,
    /// Height above the curve in metres (default 0), e.g. a wall's height.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub height: Option<f64>,
    /// Game-defined bit flags (default 0).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub tags: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineEntityParams {
    /// Entity carrying the spline (id or unique name).
    pub entity: EntityRef,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineSetParams {
    /// Entity to edit (it gets a spline if it has none).
    pub entity: EntityRef,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub kind: Option<SplineKind>,
    /// Replaces every control point (entity-local, metres).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    #[schemars(length(max = 16384))]
    pub points: Option<Vec<SplinePoint>>,
    /// Loop back to the first point.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub closed: Option<bool>,
    /// Drape samples on the terrain (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub follow_ground: Option<bool>,
    /// Metres above the ground when following it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ground_offset: Option<f64>,
    /// Curve network the spline joins (0 = none).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub network: Option<u32>,
    /// Stable id within the network (0 = the entity id).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<u32>,
}

/// Frame orientation along the curve.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum FrameMode {
    /// Up is world +Y (walls, fences).
    UpLocked,
    /// Twists as little as possible (rails, ribbons on slopes).
    RotationMinimising,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineSampleParams {
    pub entity: EntityRef,
    /// Metres between samples (default 1; at most 4096 samples).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spacing: Option<f64>,
    /// Distance of the first sample from the start (metres).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub phase: Option<f64>,
    /// Adjust the spacing so samples land on both ends.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fit_to_ends: Option<bool>,
    /// Also return `up` and `right` vectors in this orientation.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub frames: Option<FrameMode>,
    /// Drape on the terrain (default: the spline's `follow_ground`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ground: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineClosestParams {
    pub entity: EntityRef,
    /// World point [x, y, z] in metres.
    pub point: [f64; 3],
    /// Ignore height (measure in the ground plane), e.g. for a cursor hit.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub xz: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineCrossingsParams {
    pub entity: EntityRef,
    /// Second spline entity. Omit for the spline's own self-crossings.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub other: Option<EntityRef>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineOffsetParams {
    pub entity: EntityRef,
    /// Metres to the right of travel (negative: left).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub distance: Option<f64>,
    /// Plus this many half widths (1 = right edge, -1 = left edge).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub width_factor: Option<f64>,
    /// Metres between offset points (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spacing: Option<f64>,
    /// Cut away parts closer to the curve than the offset (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub trim: Option<bool>,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineNetworkParams {
    /// The splines' `network` number (> 0).
    pub network: u32,
    /// Endpoints this close (metres, seen from above) join one node
    /// (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub snap_radius: Option<f64>,
    /// A two-way node turning less than this many degrees is `straight`, else
    /// a `bend` (default 15).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub straight_deg: Option<f64>,
}

/// One stroke point: [x, y, z] in world metres, or with `straight` set when
/// the segment from the previous point was drawn straight.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum StrokePoint {
    /// A freehand point [x, y, z].
    Position([f64; 3]),
    /// A point with flags.
    Full(StrokePointFull),
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct StrokePointFull {
    /// [x, y, z] in world metres.
    pub position: [f64; 3],
    /// The segment from the previous point is straight.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub straight: Option<bool>,
}

/// What a fitted stroke becomes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum StrokeOutput {
    /// Centripetal Catmull-Rom points (the default).
    CatmullRom,
    /// The fitted Bézier pieces themselves.
    CubicBezier,
}

#[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SplineFitParams {
    /// The stroke, in drawing order (world metres).
    #[schemars(length(max = 65536))]
    pub points: Vec<StrokePoint>,
    /// Write the fitted spline to this entity (its spline is replaced; its
    /// network and id are kept). Omit to only return it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub entity: Option<EntityRef>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output: Option<StrokeOutput>,
    /// Max distance of the curve from the stroke, metres (default 0.1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fit_tolerance: Option<f64>,
    /// Hand jitter to ignore, metres (default 0.05).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub jitter_tolerance: Option<f64>,
    /// Resampling distance, metres (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sample_spacing: Option<f64>,
    /// Turns sharper than this cosine are kept as corners (default 0.5 = 60 degrees).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub corner_cos: Option<f64>,
    /// Close the loop when the stroke ends this close to its start, metres
    /// (default 0.75; 0 never closes).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub close_radius: Option<f64>,
    /// Shorter strokes are refused, metres (default 0.25).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub min_length: Option<f64>,
    /// Width of every point of the result, metres (default 1).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub width: Option<f64>,
    /// Put the points on the terrain and make the spline follow it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ground: Option<bool>,
}
