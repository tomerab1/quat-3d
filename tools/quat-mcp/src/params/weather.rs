//! Inputs of the weather tools (`weather_get`, `weather_set`;
//! docs/remote-control.md "Weather"). Ranges here are summaries; the engine's
//! `weather_get` schema is authoritative and the engine range-checks every value.

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// What falls from the sky.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum PrecipitationType {
    Rain,
    Snow,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct WeatherGetParams {
    /// Also return the field table (every field's range, default, unit and hint).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub schema: Option<bool>,
    /// World points [x, y, z] (at most 1024) to report the wind velocity at, in m/s.
    /// y is the height above the ground in metres (10 is the reference height).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_at: Option<Vec<[f64; 3]>>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct WeatherSetParams {
    /// Start from the default weather before applying the other fields.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub reset: Option<bool>,
    /// Heading the wind blows toward, degrees from +X toward +Z, -180..180 (default 35).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_dir_deg: Option<f64>,
    /// Mean wind speed 10 m above the ground in m/s, 0..60 (default 4; 10 is windy, 20 a gale).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_speed: Option<f64>,
    /// Gust fronts travelling downwind, 0..1 (default 0.35).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub gustiness: Option<f64>,
    /// Small-scale swirl of the wind, 0..1 (default 0.3).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub turbulence: Option<f64>,
    /// Fraction of the sky covered by clouds, 0..1 (default 0.42).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub cloud_cover: Option<f64>,
    /// Precipitation intensity, 0 (dry) ..1 (downpour or blizzard).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub precipitation: Option<f64>,
    /// "rain" or "snow".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub precipitation_type: Option<PrecipitationType>,
    /// Surface wetness, 0..1: darker, glossier surfaces; puddles on flat ground above ~0.55.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wetness: Option<f64>,
    /// Snow lying on upward-facing surfaces, 0..1.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub snow: Option<f64>,
    /// Air temperature in Celsius, -40..45 (default 12). Snow settles at or below 1 and melts above 0.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub temperature_c: Option<f64>,
    /// Let rain, snowfall, melt and drying change `wetness` and `snow` over simulated time (default true).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub accumulate: Option<bool>,
}

impl WeatherSetParams {
    /// True when no field would change anything.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.reset != Some(true)
            && self.wind_dir_deg.is_none()
            && self.wind_speed.is_none()
            && self.gustiness.is_none()
            && self.turbulence.is_none()
            && self.cloud_cover.is_none()
            && self.precipitation.is_none()
            && self.precipitation_type.is_none()
            && self.wetness.is_none()
            && self.snow.is_none()
            && self.temperature_c.is_none()
            && self.accumulate.is_none()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn set_serializes_only_given_fields() {
        let p: WeatherSetParams =
            serde_json::from_value(json!({"precipitation_type": "snow", "snow": 0.5})).unwrap();
        assert!(!p.is_empty());
        assert_eq!(
            serde_json::to_value(&p).unwrap(),
            json!({"precipitation_type": "snow", "snow": 0.5})
        );
        assert!(WeatherSetParams::default().is_empty());
    }

    #[test]
    fn unknown_fields_and_types_are_rejected() {
        let err = serde_json::from_value::<WeatherSetParams>(json!({"rain": 1})).unwrap_err();
        assert!(err.to_string().contains("rain"));
        assert!(
            serde_json::from_value::<WeatherSetParams>(json!({"precipitation_type": "hail"}))
                .is_err()
        );
    }
}
