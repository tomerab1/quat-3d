//! Inputs of the world-clock tools (`time_*`, docs/remote-control.md
//! "World clock").

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TimeSetParams {
    /// Date and time, ISO-8601: "2026-06-21T05:30" (local time at
    /// `utc_offset_h`), "2026-06-21T05:30:00+02:00" or "...Z" (UTC). Years
    /// 1800..2200.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub datetime: Option<String>,
    /// Local time of day "hh:mm" or "hh:mm:ss"; keeps the date.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub local_time: Option<String>,
    /// Time zone of the displayed local time: hours from UTC, -14..14 (e.g. 2, -5, 5.5).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub utc_offset_h: Option<f64>,
    /// Observer latitude in degrees, -90..90 (north positive).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub latitude_deg: Option<f64>,
    /// Observer longitude in degrees, -180..180 (east positive).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub longitude_deg: Option<f64>,
    /// Rotation of compass north in the world, degrees (0: north is -Z, east is +X).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub north_offset_deg: Option<f64>,
    /// Clock speed: clock seconds per simulated second, 0..1000000 (1 = real time, 0 = frozen, 3600 = an hour per second).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub time_scale: Option<f64>,
    /// Repeat the same day: the time of day wraps at local midnight.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub fixed_day: Option<bool>,
    /// Run the clock and let it drive the sky (false: the sun is left alone).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// Write the scene's sun light from the computed Sun position.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub drive_sun: Option<bool>,
    /// Sun light intensity with the whole Sun above the horizon (`dir_light` units), 0..100000.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sun_intensity: Option<f64>,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct TimeSkipParams {
    /// Hours to move the clock, -87600..87600 (negative goes back).
    pub hours: f64,
}
