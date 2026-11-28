//! Inputs of the season tools (`season_get`, `season_set`;
//! docs/remote-control.md "Seasons"). The engine range-checks every value
//! and names the field in its errors.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// Where the year phase comes from.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum SeasonMode {
    /// The world clock's Sun (real dates and day lengths, both hemispheres).
    Clock,
    /// Its own rate (`year_minutes`), decoupled from the day cycle.
    Independent,
    /// Held where it is set.
    Manual,
}

/// A season, by its middle.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum SeasonName {
    Spring,
    Summer,
    Autumn,
    Winter,
}

/// A weather kind; as a preset it follows the season's climate.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum WeatherPreset {
    Clear,
    Fair,
    Overcast,
    Showers,
    Rain,
    Storm,
    Snow,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SeasonGetParams {
    /// Also return the climate curves in use.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub climate: Option<bool>,
}

/// Climate curves: 1..48 values each, key i at year phase i / n (0 = the local spring equinox).
#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ClimateParams {
    /// Daily mean air temperature in Celsius, -60..50.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature_c: Option<Vec<f64>>,
    /// Daily maximum minus minimum in Celsius, 0..30.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub diurnal_range_c: Option<Vec<f64>>,
    /// Chance that a weather period brings rain or snow, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub precipitation_chance: Option<Vec<f64>>,
    /// Mean cloud cover of the dry periods, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cloud_cover: Option<Vec<f64>>,
    /// Mean wind speed 10 m above the ground in m/s, 0..40.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_speed: Option<Vec<f64>>,
    /// Standard deviation of a period's temperature anomaly in Celsius, 0..15 (default 3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature_variability_c: Option<f64>,
    /// Length of one weather period in days of the season year, 0.25..60 (default 3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weather_period_days: Option<f64>,
    /// Fraction of each period spent easing into the next, 0..1 (default 0.3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub transition: Option<f64>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct SeasonSetParams {
    /// Derive the season and drive the weather (false: everything holds).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub enabled: Option<bool>,
    /// "clock", "independent" or "manual".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mode: Option<SeasonMode>,
    /// Jump to the middle of a season (in clock mode: moves the clock's date).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub season: Option<SeasonName>,
    /// Year phase 0..1: 0 spring equinox, 0.25 summer solstice, 0.5 autumn equinox, 0.75 winter solstice.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub year_phase: Option<f64>,
    /// Season year, 1800..2199: varies the weather; anchors the driven clock date.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub year: Option<i32>,
    /// Real minutes per year at simulation speed 1 (independent mode; 40 = cozy; 0 stops).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub year_minutes: Option<f64>,
    /// Years per simulated second, 0..1 (the raw form of `year_minutes`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub phase_rate: Option<f64>,
    /// Real minutes per day: sets the world clock's `time_scale` (needs a clock).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub day_minutes: Option<f64>,
    /// Write the world clock's date from the phase (independent / manual).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub drive_clock_date: Option<bool>,
    /// Clock mode on today's real date and time at real speed.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub real_calendar: Option<bool>,
    /// Drive the weather from the climate's seeded sequence.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weather_auto: Option<bool>,
    /// Seed of the weather sequence, 0..4294967295.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weather_seed: Option<u32>,
    /// Wetness and snow evolve at the world clock's speed (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weather_clock_time: Option<bool>,
    /// Apply a weather now, adapted to the season (suspends `weather_auto`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub weather_preset: Option<WeatherPreset>,
    /// Merge these curves into the scene's climate.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub climate: Option<ClimateParams>,
    /// Back to the default climate (a cool temperate profile).
    #[serde(default, skip_serializing)]
    pub default_climate: Option<bool>,
    /// Simulate this many seconds (0..3600) up to the new state, so wet ground and snow show.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub spin_up_s: Option<f64>,
}
