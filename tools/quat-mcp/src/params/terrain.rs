//! Inputs of the terrain edit tools (`terrain_edit_*`; docs/remote-control.md
//! "Terrain edits"). Coordinates are world space; ranges here are summaries and
//! the engine validates every field, naming the one it rejects.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Most points one stamp may carry (contract limit).
pub const MAX_STAMP_POINTS: usize = 256;

/// The stamp's footprint.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum StampShape {
    /// One point: the centre; `radius` is the circle's radius.
    Circle,
    /// A polyline of 1+ points swept by a disc of `radius` (paths, roads, ditches).
    Capsule,
    /// A closed ring of 3+ points, grown outward by `radius` (plinths, yards).
    Polygon,
}

/// What the stamp does to the ground inside its footprint.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum StampOp {
    /// Blend toward a target height (level, or the plane given by `normal`).
    Flatten,
    /// Add `amount` metres.
    Raise,
    /// Remove `amount` metres.
    Lower,
    /// Blur the heights (`iterations` passes).
    Smooth,
    /// Paint a surface channel toward `value`.
    Paint,
}

/// The surface layer a `paint` stamp writes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum PaintChannel {
    Dirt,
    Gravel,
    Soil,
    /// The grass-density mask: 0 clears grass, 1 lets it grow.
    Grass,
}

/// Declares a stamp parameter struct: the given fields followed by the stamp
/// fields every add / update shares (one definition, so the two cannot drift).
macro_rules! stamp_params {
    ($(#[$attr:meta])* pub struct $name:ident { $($fields:tt)* }) => {
        $(#[$attr])*
        #[derive(Debug, Clone, Serialize, Deserialize, JsonSchema)]
        #[serde(deny_unknown_fields)]
        pub struct $name {
            $($fields)*
            /// Circle radius / capsule half-width / polygon outset, metres (default 0).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub radius: Option<f64>,
            /// Metres beyond the shape over which the effect fades to 0 (smoothstep;
            /// default 0). `radius + falloff` is at most 4096.
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub falloff: Option<f64>,
            /// Blend toward the op's target, 0..1 (default 1).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub strength: Option<f64>,
            /// flatten: target height (world Y, metres) at `pivot`.
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub height: Option<f64>,
            /// flatten: world [x, z] where the target plane has `height` (matters only
            /// with a tilted `normal`).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub pivot: Option<[f64; 2]>,
            /// flatten: upward [x, y, z] normal of the target plane (default [0, 1, 0]: level).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub normal: Option<[f64; 3]>,
            /// raise / lower: metres, 0..1000.
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub amount: Option<f64>,
            /// smooth: 3x3 binomial passes, 1..16 (default 1).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub iterations: Option<u32>,
            /// paint: "dirt", "gravel", "soil" or "grass" (the grass-density mask).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub channel: Option<PaintChannel>,
            /// paint: the channel's target value, 0..1 (default 1; grass 0 clears grass).
            #[serde(default, skip_serializing_if = "Option::is_none")]
            pub value: Option<f64>,
        }
    };
}

stamp_params! {
    pub struct TerrainEditAddParams {
        /// "circle" (one point), "capsule" (polyline of 1+ points) or "polygon" (ring of 3+ points).
        pub shape: StampShape,
        /// World-space [x, z] points in metres, at most 256.
        pub points: Vec<[f64; 2]>,
        /// "flatten", "raise", "lower", "smooth" or "paint".
        pub op: StampOp,
        /// Insert before the stamp with this id (stamps apply in order); omit to append.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pub before: Option<u64>,
    }
}

stamp_params! {
    pub struct TerrainEditUpdateParams {
        /// Id of the stamp to change (from `terrain_edit_add` / `terrain_edit_list`).
        pub id: u64,
        /// New shape; omit to keep it.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pub shape: Option<StampShape>,
        /// New world-space [x, z] points in metres (at most 256); omit to keep them.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pub points: Option<Vec<[f64; 2]>>,
        /// New op; omit to keep it.
        #[serde(default, skip_serializing_if = "Option::is_none")]
        pub op: Option<StampOp>,
    }
}

impl TerrainEditUpdateParams {
    /// True when no field besides `id` is given.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.shape.is_none()
            && self.points.is_none()
            && self.op.is_none()
            && self.radius.is_none()
            && self.falloff.is_none()
            && self.strength.is_none()
            && self.height.is_none()
            && self.pivot.is_none()
            && self.normal.is_none()
            && self.amount.is_none()
            && self.iterations.is_none()
            && self.channel.is_none()
            && self.value.is_none()
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TerrainEditRemoveParams {
    /// One stamp id to remove.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub id: Option<u64>,
    /// Several stamp ids to remove (instead of `id`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ids: Option<Vec<u64>>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn add_serializes_only_given_fields() {
        let request = json!({"shape": "capsule", "points": [[0.0, 0.0], [12.0, 3.0]],
            "op": "paint", "radius": 1.2, "channel": "grass", "value": 0.0});
        let p: TerrainEditAddParams = serde_json::from_value(request.clone()).unwrap();
        assert_eq!(p.channel, Some(PaintChannel::Grass));
        assert_eq!(serde_json::to_value(&p).unwrap(), request);
    }

    #[test]
    fn update_knows_when_it_changes_nothing() {
        let p: TerrainEditUpdateParams = serde_json::from_value(json!({"id": 3})).unwrap();
        assert!(p.is_empty());
        let p: TerrainEditUpdateParams =
            serde_json::from_value(json!({"id": 3, "height": 2})).unwrap();
        assert!(!p.is_empty());
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"id": 3, "height": 2.0})
        );
    }

    #[test]
    fn unknown_fields_and_names_are_rejected() {
        let base = json!({"shape": "circle", "points": [[0, 0]], "op": "raise"});
        assert!(serde_json::from_value::<TerrainEditAddParams>(base.clone()).is_ok());
        let mut extra = base.clone();
        extra["depth"] = json!(1);
        let err = serde_json::from_value::<TerrainEditAddParams>(extra).unwrap_err();
        assert!(err.to_string().contains("depth"));
        let mut bad_op = base;
        bad_op["op"] = json!("dig");
        assert!(serde_json::from_value::<TerrainEditAddParams>(bad_op).is_err());
        assert!(
            serde_json::from_value::<TerrainEditUpdateParams>(json!({"id": 1, "channel": "sand"}))
                .is_err()
        );
    }
}
