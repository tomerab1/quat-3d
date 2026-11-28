// ECS <-> physics bridge system (Phase 6, Slice 6.2).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/event_ring.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/scene/dynamic_mesh.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/scene/scene.hpp"

namespace engine::scene {

namespace {

// Builds the Jolt shape with the entity's world scale baked in, so the shape
// matches what the editor's collider wireframe shows (the overlay draws
// through the scaled world matrix). Spheres/capsules cannot scale
// non-uniformly in Jolt; they take the dominant axes.
[[nodiscard]] std::uint32_t make_shape(physics::PhysicsWorld& world, const Collider& col,
                                       const glm::vec3& scale) {
    switch (col.shape) {
    case ColliderShape::box:
        return world.create_box(col.half_extents * scale);
    case ColliderShape::sphere:
        return world.create_sphere(col.half_extents.x *
                                   std::max({scale.x, scale.y, scale.z}));
    case ColliderShape::capsule:
        return world.create_capsule(col.half_extents.y * scale.y,
                                    col.half_extents.x * std::max(scale.x, scale.z));
    }
    return world.create_box(col.half_extents * scale);
}

[[nodiscard]] physics::Motion to_motion(BodyMotion m) {
    switch (m) {
    case BodyMotion::static_body:
        return physics::Motion::static_body;
    case BodyMotion::kinematic:
        return physics::Motion::kinematic;
    case BodyMotion::dynamic:
        return physics::Motion::dynamic;
    }
    return physics::Motion::dynamic;
}

// Bodies live in WORLD space: entities with colliders may sit anywhere in the
// hierarchy (e.g. glTF mesh children), so Transform.world — not local — is the
// simulation pose. Rotation is extracted scale-free.
[[nodiscard]] glm::vec3 position_of(const Transform& t) { return glm::vec3(t.world[3]); }
[[nodiscard]] glm::quat rotation_of(const Transform& t) {
    glm::mat3 m(t.world);
    m[0] = glm::normalize(m[0]);
    m[1] = glm::normalize(m[1]);
    m[2] = glm::normalize(m[2]);
    return glm::quat_cast(m);
}
[[nodiscard]] glm::vec3 scale_of(const Transform& t) {
    return {glm::length(glm::vec3(t.world[0])), glm::length(glm::vec3(t.world[1])),
            glm::length(glm::vec3(t.world[2]))};
}
// The collider offset in world units, rotated into the entity's frame — the
// body's centre is the entity position plus this.
[[nodiscard]] glm::vec3 world_offset(const Transform& t, const Collider& col) {
    return rotation_of(t) * (scale_of(t) * col.offset);
}

} // namespace

std::uint32_t ensure_rigid_body(entt::registry& registry, physics::PhysicsWorld& world,
                                entt::entity e) {
    if (!registry.valid(e)) return physics::PhysicsWorld::invalid_body;
    auto* rb = registry.try_get<RigidBody>(e);
    const auto* col = registry.try_get<Collider>(e);
    const auto* t = registry.try_get<Transform>(e);
    if (rb == nullptr || col == nullptr || t == nullptr) return physics::PhysicsWorld::invalid_body;
    if (rb->body != physics::PhysicsWorld::invalid_body) return rb->body;
    physics::PhysicsWorld::BodyParams params;
    params.shape = make_shape(world, *col, scale_of(*t));
    params.position = position_of(*t) + world_offset(*t, *col);
    params.rotation = rotation_of(*t);
    params.motion = to_motion(rb->motion);
    params.sensor = col->is_sensor;
    params.layer = col->is_sensor ? physics::Layer::sensor
                   : rb->motion == BodyMotion::static_body ? physics::Layer::static_body
                                                           : physics::Layer::dynamic;
    params.mass = rb->mass;
    params.friction = rb->friction;
    params.restitution = rb->restitution;
    params.gravity_factor = rb->gravity_factor;
    params.linear_damping = rb->linear_damping;
    params.angular_damping = rb->angular_damping;
    params.owner = static_cast<std::uint32_t>(entt::to_integral(e));
    rb->body = world.add_body(params);
    // The body owns its shape; a shape that backs no body is given back.
    if (rb->body == physics::PhysicsWorld::invalid_body) world.release_shape(params.shape);
    return rb->body;
}

void physics_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    const std::uint32_t invalid = physics::PhysicsWorld::invalid_body;
    world.set_gravity(physics_settings(registry).gravity); // the scene's one setting

    // 0. Remove bodies whose owner is gone: the entity was destroyed, or it
    // lost its RigidBody / Collider (editor delete or remove-component during
    // Play, scene restore). A surviving RigidBody is reset so re-adding the
    // Collider creates a fresh body instead of leaking a second one.
    for (const physics::PhysicsWorld::OwnedBody& ob : world.owned_bodies()) {
        const auto e = static_cast<entt::entity>(ob.owner);
        auto* rb = registry.valid(e) ? registry.try_get<RigidBody>(e) : nullptr;
        const bool live = rb != nullptr && rb->body == ob.body &&
                          registry.all_of<Collider, Transform>(e);
        if (live) continue;
        // Static mesh colliders of DynamicMesh entities (R23.2) are kept by
        // dynamic_mesh_collider_system.
        const auto* dm = registry.valid(e) ? registry.try_get<DynamicMesh>(e) : nullptr;
        if (dm != nullptr && dm->body == ob.body) continue;
        world.remove_body(ob.body);
        if (rb != nullptr && rb->body == ob.body) rb->body = invalid;
    }

    // 1. Create a body for each entity that has a Collider but no body yet.
    for (auto [e, rb, col, t] : registry.view<RigidBody, Collider, Transform>().each()) {
        if (rb.body == invalid) (void)ensure_rigid_body(registry, world, e);
    }

    // 1b. Joints between the (possibly just rebuilt) bodies.
    joint_system(registry, world);

    // 2. Push kinematic bodies' transforms into the simulation.
    for (auto [e, rb, col, t] : registry.view<RigidBody, Collider, Transform>().each()) {
        if (rb.motion == BodyMotion::kinematic && rb.body != invalid) {
            world.set_body_transform(rb.body, position_of(t) + world_offset(t, col),
                                     rotation_of(t));
        }
    }

    // 3. Step the simulation.
    world.update(dt);

    // 4. Pull dynamic bodies' world poses back into the ECS. The body centre
    // carries the collider offset, so it is subtracted (in the body's current
    // orientation) to recover the entity position. The entity's authored scale
    // is preserved, and parented entities convert the pose through the
    // parent's world so the transform system reproduces it.
    for (auto [e, rb, col, t] : registry.view<RigidBody, Collider, Transform>().each()) {
        if (rb.motion == BodyMotion::dynamic && rb.body != invalid) {
            glm::vec3 pos(0.0F);
            glm::quat rot(1.0F, 0.0F, 0.0F, 0.0F);
            world.body_transform(rb.body, pos, rot);
            const glm::vec3 scale = scale_of(t);
            pos -= rot * (scale * col.offset);
            set_world_transform(registry, e,
                                glm::translate(glm::mat4(1.0F), pos) * glm::mat4_cast(rot) *
                                    glm::scale(glm::mat4(1.0F), scale));
        }
    }
}

void publish_physics_events(const physics::PhysicsWorld& world, core::EventRing& ring) {
    using Kind = physics::PhysicsWorld::ContactEvent::Kind;
    const auto entity_of = [](std::uint32_t owner) {
        // Owners are entity ids (physics_system / character_system pass
        // entt::to_integral); unowned bodies are terrain or demo floors.
        return owner == physics::PhysicsWorld::no_owner ? core::Event::no_entity : owner;
    };
    const double dt = physics::PhysicsWorld::fixed_timestep;
    for (const physics::PhysicsWorld::ContactEvent& c : world.contact_events()) {
        core::Event ev;
        switch (c.kind) {
        case Kind::contact_added:
            ev.kind = core::EventKind::contact_added;
            break;
        case Kind::contact_removed:
            ev.kind = core::EventKind::contact_removed;
            break;
        case Kind::trigger_enter:
            ev.kind = core::EventKind::trigger_enter;
            break;
        case Kind::trigger_exit:
            ev.kind = core::EventKind::trigger_exit;
            break;
        }
        ev.time_s = static_cast<double>(c.step) * dt;
        ev.entity_a = entity_of(c.owner_a);
        ev.entity_b = entity_of(c.owner_b);
        ev.point = {c.point.x, c.point.y, c.point.z};
        ev.normal = {c.normal.x, c.normal.y, c.normal.z};
        ev.impulse = c.impulse;
        ring.push(ev);
    }
    if (const std::uint32_t lost = world.dropped_contact_events(); lost > 0) ring.drop(lost);
}

namespace {

// Remote driving (character.move): walk towards a target point, stop when a
// timed walk runs out. Writes `move` like a player's input would.
void steer_character(CharacterController& cc, const glm::vec3& position, float dt) {
    if (cc.has_move_target) {
        const glm::vec2 to(cc.move_target.x - position.x, cc.move_target.z - position.z);
        const float dist = glm::length(to);
        const float step = cc.speed * dt;
        if (dist <= 0.02F || cc.speed <= 0.0F) {
            cc.move = glm::vec3(0.0F);
            cc.has_move_target = false; // arrived
        } else {
            // Slow down on the last step instead of overshooting.
            const float scale = step > dist ? dist / step : 1.0F;
            cc.move = glm::vec3(to.x / dist, 0.0F, to.y / dist) * scale;
        }
    }
    if (cc.move_time_left >= 0.0F) {
        cc.move_time_left -= dt;
        if (cc.move_time_left <= 0.0F) {
            cc.move = glm::vec3(0.0F);
            cc.has_move_target = false;
            cc.move_time_left = -1.0F;
        }
    }
}

void advance_character(entt::registry& registry, entt::entity e, CharacterController& cc,
                       const Transform& t, physics::PhysicsWorld& world, float dt) {
    // Characters stand along +Y: they fall with the vertical component of the
    // world gravity (the scene's PhysicsSettings, shared with Jolt).
    const float gravity = world.gravity().y;
    if (cc.character == physics::PhysicsWorld::invalid_body) {
        cc.character = world.create_character(cc.half_height, cc.radius, position_of(t),
                                              static_cast<std::uint32_t>(entt::to_integral(e)));
    }

    steer_character(cc, world.character_position(cc.character), dt);

    glm::vec3 velocity = cc.velocity;
    if (cc.on_ground && velocity.y * gravity > 0.0F) {
        velocity.y = 0.0F; // grounded: stop accumulating speed along gravity
    }
    if (cc.jump_speed > 0.0F) { // one request, honoured only from the ground
        if (cc.on_ground) velocity.y = gravity <= 0.0F ? cc.jump_speed : -cc.jump_speed;
        cc.jump_speed = 0.0F;
    }
    velocity.y += gravity * dt;
    velocity.x = cc.move.x * cc.speed;
    velocity.z = cc.move.z * cc.speed;

    world.character_set_velocity(cc.character, velocity);
    world.character_update(cc.character, dt);

    cc.velocity = world.character_velocity(cc.character);
    cc.on_ground = world.character_on_ground(cc.character);
    // The controller moves the entity's origin only: keep its world rotation
    // and scale (a capsule never turns) and its parent's space.
    glm::mat4 pose = t.world;
    pose[3] = glm::vec4(world.character_position(cc.character), 1.0F);
    set_world_transform(registry, e, pose);
}

} // namespace

void character_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    world.set_gravity(physics_settings(registry).gravity);
    // Remove characters whose entity or CharacterController is gone (see
    // physics_system step 0).
    for (const physics::PhysicsWorld::OwnedBody& oc : world.owned_characters()) {
        const auto e = static_cast<entt::entity>(oc.owner);
        auto* cc = registry.valid(e) ? registry.try_get<CharacterController>(e) : nullptr;
        if (cc != nullptr && cc->character == oc.body) continue;
        world.remove_character(oc.body);
    }

    for (auto [e, cc, t] :
         registry.view<CharacterController, Transform>(entt::exclude<NetDriven>).each()) {
        advance_character(registry, e, cc, t, world, dt);
    }
}

void step_character(entt::registry& registry, physics::PhysicsWorld& world, entt::entity e,
                    float dt) {
    if (!registry.valid(e)) return;
    world.set_gravity(physics_settings(registry).gravity);
    auto* cc = registry.try_get<CharacterController>(e);
    auto* t = registry.try_get<Transform>(e);
    if (cc == nullptr || t == nullptr) return;
    advance_character(registry, e, *cc, *t, world, dt);
}

std::uint32_t add_height_field_body(physics::PhysicsWorld& world, const terrain::Heightmap& map,
                                    const glm::vec3& origin) {
    if (!map.valid()) return physics::PhysicsWorld::invalid_body;
    // Jolt wants the sample count block-aligned (multiple of 2); our grids are
    // 2^n + 1, so pad one duplicated row/column. Interior cells then align
    // exactly with the rendered texels; the pad adds a flat one-texel apron
    // beyond the tile edge.
    const std::uint32_t res = map.resolution;
    const std::uint32_t padded = res % 2 == 0 ? res : res + 1;
    std::vector<float> samples(static_cast<std::size_t>(padded) * padded);
    for (std::uint32_t z = 0; z < padded; ++z) {
        const std::uint32_t sz = std::min(z, res - 1);
        for (std::uint32_t x = 0; x < padded; ++x) {
            const std::uint32_t sx = std::min(x, res - 1);
            samples[static_cast<std::size_t>(z) * padded + x] =
                map.heights[static_cast<std::size_t>(sz) * res + sx];
        }
    }

    const float texel = map.metres_per_texel();
    const std::uint32_t shape = world.create_height_field(
        samples, padded, glm::vec3(0.0F), glm::vec3(texel, 1.0F, texel));
    if (shape == physics::PhysicsWorld::invalid_body) {
        return physics::PhysicsWorld::invalid_body;
    }
    physics::PhysicsWorld::BodyParams params;
    params.shape = shape;
    params.position = origin;
    params.motion = physics::Motion::static_body;
    params.layer = physics::Layer::static_body;
    const std::uint32_t body = world.add_body(params);
    if (body == physics::PhysicsWorld::invalid_body) world.release_shape(shape);
    return body; // removing the body releases its height field
}

void collect_nav_geometry(entt::registry& registry, const terrain::Heightmap* map,
                          const glm::vec3& anchor, std::vector<glm::vec3>& verts,
                          std::vector<std::uint32_t>& indices, bool include_colliders) {
    if (map != nullptr && map->valid()) {
        // Decimate the grid so even 1k maps stay around ~32k triangles.
        const std::uint32_t res = map->resolution;
        const std::uint32_t step = std::max(1U, (res - 1) / 128U);
        const std::uint32_t cells = (res - 1) / step;
        const float texel = map->metres_per_texel();
        const glm::vec3 origin(anchor.x - map->tile_size_m * 0.5F, anchor.y,
                               anchor.z - map->tile_size_m * 0.5F);
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (std::uint32_t z = 0; z <= cells; ++z) {
            for (std::uint32_t x = 0; x <= cells; ++x) {
                const std::uint32_t sx = std::min(x * step, res - 1);
                const std::uint32_t sz = std::min(z * step, res - 1);
                verts.emplace_back(origin.x + static_cast<float>(sx) * texel,
                                   origin.y + map->at(static_cast<std::int32_t>(sx),
                                                      static_cast<std::int32_t>(sz)),
                                   origin.z + static_cast<float>(sz) * texel);
            }
        }
        const std::uint32_t stride = cells + 1;
        for (std::uint32_t z = 0; z < cells; ++z) {
            for (std::uint32_t x = 0; x < cells; ++x) {
                const std::uint32_t i00 = base + z * stride + x;
                indices.insert(indices.end(), {i00, i00 + stride, i00 + 1, i00 + 1, i00 + stride,
                                               i00 + stride + 1});
            }
        }
    }

    if (!include_colliders) return;
    // Static box colliders become 12 triangles each (the world matrix carries
    // scale/rotation, matching the simulated shape closely enough for nav).
    // Triangles are wound counter-clockwise seen from outside, so every
    // face's (v1 - v0) x (v2 - v0) normal points away from the box (the same
    // convention as the terrain grid above): Recast's slope filter keeps the
    // top faces walkable and rejects the bottoms.
    for (auto [e, t, col] : registry.view<const Transform, const Collider>().each()) {
        if (col.shape != ColliderShape::box || col.is_sensor) continue;
        // Agents and characters move: their own collider is not level
        // geometry (baking it would punch a hole where the agent stands).
        if (registry.any_of<NavAgent, CharacterController>(e)) continue;
        const auto* body = registry.try_get<RigidBody>(e);
        if (body != nullptr && body->motion != BodyMotion::static_body) continue;
        const auto base = static_cast<std::uint32_t>(verts.size());
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 sign(((corner & 1) != 0) ? 1.0F : -1.0F,
                                 ((corner & 2) != 0) ? 1.0F : -1.0F,
                                 ((corner & 4) != 0) ? 1.0F : -1.0F);
            verts.emplace_back(
                glm::vec3(t.world * glm::vec4(col.offset + col.half_extents * sign, 1.0F)));
        }
        // Corner bits: 1 = +x, 2 = +y, 4 = +z.
        constexpr std::array<std::uint32_t, 36> box_idx{
            0, 3, 1, 0, 2, 3,  // -z
            4, 7, 6, 4, 5, 7,  // +z
            0, 6, 2, 0, 4, 6,  // -x
            1, 7, 5, 1, 3, 7,  // +x
            0, 5, 4, 0, 1, 5,  // -y (bottom)
            2, 7, 3, 2, 6, 7}; // +y (top)
        for (const std::uint32_t i : box_idx) indices.push_back(base + i);
    }
}

std::size_t promote_collider_only_bodies(entt::registry& registry) {
    std::vector<std::pair<entt::entity, BodyMotion>> promote;
    for (const entt::entity e : registry.view<Collider>(entt::exclude<RigidBody>)) {
        // A character's capsule controller is its physics presence; a static
        // body on it would be a wall the character is stuck inside.
        if (registry.all_of<CharacterController>(e)) continue;
        // A kinematic nav agent moves through the ECS: it gets a kinematic
        // body (pushed from its Transform each step), never a static wall.
        promote.emplace_back(e, registry.all_of<NavAgent>(e) ? BodyMotion::kinematic
                                                             : BodyMotion::static_body);
    }
    for (const auto& [e, motion] : promote) {
        registry.emplace<RigidBody>(e, motion, 0.0F);
    }
    return promote.size();
}

std::uint32_t add_terrain_body(entt::registry& registry, physics::PhysicsWorld& world,
                               const terrain::Heightmap& map) {
    const auto view = registry.view<const Terrain>();
    if (view.begin() == view.end()) return physics::PhysicsWorld::invalid_body;
    const auto* tf = registry.try_get<Transform>(*view.begin());
    const glm::vec3 pos = tf != nullptr ? glm::vec3(tf->world[3]) : glm::vec3(0.0F);
    const float tile = map.tile_size_m;
    return add_height_field_body(world, map,
                                 glm::vec3(pos.x - tile * 0.5F, pos.y, pos.z - tile * 0.5F));
}

} // namespace engine::scene
