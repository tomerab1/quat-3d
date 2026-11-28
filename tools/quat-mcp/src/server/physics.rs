//! Physics control tools (ROADMAP A10.1): body state, velocities and
//! impulses, the scene gravity, multi-hit queries, the contact / trigger
//! event feed and character movement. Every tool maps 1:1 to an engine
//! method (docs/remote-control.md "Physics control"); descriptions live in
//! `content/tools/<tool>.md`. All registrations are in this one module so
//! the router gains a single line.

use rmcp::handler::server::wrapper::Parameters;
use rmcp::model::CallToolResult;
use rmcp::{tool, tool_router};

use super::QuatServer;
use super::results::{invalid_input, to_params};
use crate::params::{
    CharacterMoveParams, EntityParams, EventsPollParams, GravityParams, OverlapParams,
    PhysicsApplyParams, PhysicsSetParams, RaycastAllParams, ShapeCastParams,
};

#[tool_router(router = physics_tools, vis = "pub(super)")]
impl QuatServer {
    #[doc = include_str!("../../content/tools/events_poll.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn events_poll(&self, params: Parameters<EventsPollParams>) -> CallToolResult {
        self.run("events.poll", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/physics_get.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn physics_get(&self, params: Parameters<EntityParams>) -> CallToolResult {
        self.run("physics.get", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/physics_set.md")]
    #[tool(annotations(idempotent_hint = true, destructive_hint = false))]
    async fn physics_set(&self, params: Parameters<PhysicsSetParams>) -> CallToolResult {
        if params.0.is_empty() {
            return invalid_input(
                "physics_set: nothing to change; give at least one of linear_velocity, \
                 angular_velocity, motion_type, gravity_factor, friction, restitution, \
                 linear_damping, angular_damping or wake.",
            );
        }
        self.run("physics.set", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/physics_apply.md")]
    #[tool(annotations(destructive_hint = false, idempotent_hint = false))]
    async fn physics_apply(&self, params: Parameters<PhysicsApplyParams>) -> CallToolResult {
        if params.0.kinds() != 1 {
            return invalid_input(
                "physics_apply: give exactly one of impulse, force, angular_impulse or torque.",
            );
        }
        self.run("physics.apply", Some(to_params(&params.0))).await
    }

    #[doc = include_str!("../../content/tools/physics_get_gravity.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn physics_get_gravity(&self) -> CallToolResult {
        self.run("physics.get_gravity", None).await
    }

    #[doc = include_str!("../../content/tools/physics_set_gravity.md")]
    #[tool(annotations(idempotent_hint = true, destructive_hint = false))]
    async fn physics_set_gravity(&self, params: Parameters<GravityParams>) -> CallToolResult {
        self.run("physics.set_gravity", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/physics_raycast_all.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn physics_raycast_all(&self, params: Parameters<RaycastAllParams>) -> CallToolResult {
        if params.0.direction.iter().all(|c| *c == 0.0) {
            return invalid_input(
                "physics_raycast_all: `direction` is [0, 0, 0]; give the direction to cast in, \
                 e.g. [0, -1, 0] for straight down.",
            );
        }
        self.run("physics.raycast_all", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/physics_overlap.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn physics_overlap(&self, params: Parameters<OverlapParams>) -> CallToolResult {
        self.run("physics.overlap", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/physics_shape_cast.md")]
    #[tool(annotations(read_only_hint = true))]
    async fn physics_shape_cast(&self, params: Parameters<ShapeCastParams>) -> CallToolResult {
        let p = &params.0;
        let length_sq: f64 = p
            .from
            .iter()
            .zip(p.to)
            .map(|(a, b)| (b - a) * (b - a))
            .sum();
        if length_sq < 1e-8 {
            return invalid_input(
                "physics_shape_cast: `from` equals `to`; use physics_overlap for a shape at rest.",
            );
        }
        self.run("physics.shape_cast", Some(to_params(&params.0)))
            .await
    }

    #[doc = include_str!("../../content/tools/character_move.md")]
    #[tool(annotations(destructive_hint = false, idempotent_hint = false))]
    async fn character_move(&self, params: Parameters<CharacterMoveParams>) -> CallToolResult {
        let p = &params.0;
        if p.velocity.is_some() && p.target.is_some() {
            return invalid_input(
                "character_move: give `velocity` or `target`, not both (a target walks at the \
                 controller's speed).",
            );
        }
        if p.velocity.is_none() && p.target.is_none() && p.jump.is_none() {
            return invalid_input(
                "character_move: nothing to do; give `velocity`, `target` and/or `jump`.",
            );
        }
        self.run("character.move", Some(to_params(p))).await
    }
}
