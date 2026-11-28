//! Inputs of the game-packaging tools (`project_*`; docs/remote-control.md
//! "Game projects and packaging").

use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

/// The OS a package is for. The engine packages for its own OS only.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
pub enum PackageTarget {
    #[serde(rename = "linux-x64")]
    LinuxX64,
    #[serde(rename = "windows-x64")]
    WindowsX64,
    #[serde(rename = "macos-arm64")]
    MacosArm64,
}

/// Package configuration.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "lowercase")]
pub enum PackageConfig {
    /// Remote control stays off in the packaged game.
    Shipping,
    /// The packaged game accepts `QUAT_REMOTE`, so MCP can drive it.
    Development,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ProjectBuildParams {
    /// Target OS; must be the engine's own (see `project_info.host_target`). Omit for it.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub target: Option<PackageTarget>,
    /// Output folder, relative to the game project (default "dist").
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output: Option<String>,
    /// "shipping" (default) or "development".
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub config: Option<PackageConfig>,
    /// Launch the package headless afterwards and check it runs (slower).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub smoke_test: Option<bool>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ProjectBuildStatusParams {
    /// How many of the build log's last lines to include, 0..1000 (default 40).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub log_lines: Option<u32>,
}
