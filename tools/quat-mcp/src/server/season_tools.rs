//! Season tools (ROADMAP R25.1): the year phase, the accelerated or real
//! calendar and the climate that drives the weather. Descriptions live in
//! `content/tools/<tool>.md`; the engine methods are documented in
//! docs/remote-control.md ("Seasons").

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};
use serde_json::Value;

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{SeasonGetParams, SeasonSetParams};

#[tool_router(router = season_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/season_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn season_get(&self, params: Parameters<SeasonGetParams>) -> CallToolResult {
        self.run("season.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/season_set.md")]
    #[tool]
    async fn season_set(&self, params: Parameters<SeasonSetParams>) -> CallToolResult {
        let p = params.0;
        let mut args = to_params(&p);
        if p.default_climate == Some(true) {
            if p.climate.is_some() {
                return invalid_input(
                    "season_set: give either climate (curves to merge) or default_climate: true, \
                     not both.",
                );
            }
            // The engine's spelling of "back to the default climate".
            if let Value::Object(map) = &mut args {
                map.insert("climate".to_owned(), Value::Null);
            }
        }
        if p.season.is_some() && p.year_phase.is_some() {
            return invalid_input(
                "season_set: give either season (the middle of one) or year_phase, not both.",
            );
        }
        self.run("season.set", Some(args)).await
    }
}
