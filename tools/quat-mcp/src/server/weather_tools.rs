//! Weather tools (ROADMAP R13.1): wind, clouds, rain / snow, wet and snowy
//! surfaces. Descriptions live in `content/tools/<tool>.md`; the engine
//! methods are documented in docs/remote-control.md ("Weather").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{WeatherGetParams, WeatherSetParams};

/// Most `wind_at` points one `weather_get` call may carry (contract limit).
const MAX_WIND_POINTS: usize = 1024;

#[tool_router(router = weather_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/weather_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn weather_get(&self, params: Parameters<WeatherGetParams>) -> CallToolResult {
        let n = params.0.wind_at.as_ref().map_or(0, Vec::len);
        if n > MAX_WIND_POINTS {
            return invalid_input(format!(
                "weather_get: {n} wind_at points given, at most {MAX_WIND_POINTS} per call; \
                 split the query."
            ));
        }
        self.run("weather.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/weather_set.md")]
    #[tool(annotations(idempotent_hint = true))]
    async fn weather_set(&self, params: Parameters<WeatherSetParams>) -> CallToolResult {
        if params.0.is_empty() {
            return invalid_input(
                "weather_set: nothing to change; give at least one field, e.g. \
                 {\"precipitation\": 0.7, \"wetness\": 0.8}, or {\"reset\": true}.",
            );
        }
        self.run("weather.set", Some(to_params(&params.0))).await
    }
}
