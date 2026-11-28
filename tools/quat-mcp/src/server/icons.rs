//! Icons for the server and its tools: one small SVG per tool group in
//! `icons/`, drawn with `currentColor` and served as data URIs in a light-
//! and a dark-theme colour, so clients need no network access to show them.

use std::collections::BTreeMap;
use std::sync::LazyLock;

use base64::Engine as _;
use base64::engine::general_purpose::STANDARD;
use rmcp::model::{Icon, IconTheme};

/// The tool groups that share an icon.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Group {
    /// The server itself.
    Server,
    Engine,
    Scene,
    World,
    Camera,
    Sim,
    Debug,
    Behavior,
    Physics,
    Code,
    Render,
    Asset,
    Project,
}

const SVGS: &[(Group, &str)] = &[
    (Group::Server, include_str!("../../icons/quat.svg")),
    (Group::Engine, include_str!("../../icons/engine.svg")),
    (Group::Scene, include_str!("../../icons/scene.svg")),
    (Group::World, include_str!("../../icons/world.svg")),
    (Group::Camera, include_str!("../../icons/camera.svg")),
    (Group::Sim, include_str!("../../icons/sim.svg")),
    (Group::Debug, include_str!("../../icons/debug.svg")),
    (Group::Behavior, include_str!("../../icons/behavior.svg")),
    (Group::Physics, include_str!("../../icons/physics.svg")),
    (Group::Code, include_str!("../../icons/code.svg")),
    (Group::Render, include_str!("../../icons/render.svg")),
    (Group::Asset, include_str!("../../icons/asset.svg")),
    (Group::Project, include_str!("../../icons/project.svg")),
];

/// Stroke colours: dark strokes for light themes and the reverse.
const THEMES: [(IconTheme, &str); 2] =
    [(IconTheme::Light, "#1f2937"), (IconTheme::Dark, "#e5e7eb")];

static ICONS: LazyLock<BTreeMap<Group, Vec<Icon>>> = LazyLock::new(|| {
    SVGS.iter()
        .map(|(group, svg)| {
            let icons = THEMES
                .iter()
                .map(|(theme, colour)| {
                    let svg = svg.trim().replace("currentColor", colour);
                    Icon::new(format!(
                        "data:image/svg+xml;base64,{}",
                        STANDARD.encode(svg)
                    ))
                    .with_mime_type("image/svg+xml")
                    .with_sizes(vec!["any".to_owned()])
                    .with_theme(*theme)
                })
                .collect();
            (*group, icons)
        })
        .collect()
});

/// The light and dark icons of `group`.
#[must_use]
pub fn icons(group: Group) -> &'static [Icon] {
    ICONS.get(&group).map_or(&[], Vec::as_slice)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_group_has_a_themed_svg() {
        for (group, svg) in SVGS {
            assert!(svg.trim_start().starts_with("<svg") && svg.contains("currentColor"));
            let icons = icons(*group);
            assert_eq!(icons.len(), 2, "{group:?}");
            let decoded = STANDARD
                .decode(
                    icons[0]
                        .src
                        .trim_start_matches("data:image/svg+xml;base64,"),
                )
                .expect("base64");
            let decoded = String::from_utf8(decoded).expect("utf-8");
            assert!(decoded.contains("#1f2937") && !decoded.contains("currentColor"));
        }
        assert_eq!(SVGS.len(), 13, "one SVG per group");
    }
}
