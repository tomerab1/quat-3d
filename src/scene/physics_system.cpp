// ECS <-> physics bridge system (Phase 6, Slice 6.2).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/physics_world.hpp"
#include "engine/scene/components.hpp"
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

void physics_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    const std::uint32_t invalid = physics::PhysicsWorld::invalid_body;

    // 1. Create a body for each entity that has a Collider but no body yet.
    for (auto [e, rb, col, t] : registry.view<RigidBody, Collider, Transform>().each()) {
        if (rb.body != invalid) continue;
        physics::PhysicsWorld::BodyParams params;
        params.shape = make_shape(world, col, scale_of(t));
        params.position = position_of(t) + world_offset(t, col);
        params.rotation = rotation_of(t);
        params.motion = to_motion(rb.motion);
        params.sensor = col.is_sensor;
        params.layer = col.is_sensor ? physics::Layer::sensor
                       : rb.motion == BodyMotion::static_body ? physics::Layer::static_body
                                                              : physics::Layer::dynamic;
        params.mass = rb.mass;
        rb.body = world.add_body(params);
    }

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
            const glm::mat4 pose = glm::translate(glm::mat4(1.0F), pos) * glm::mat4_cast(rot) *
                                   glm::scale(glm::mat4(1.0F), scale);

            glm::mat4 parent_world(1.0F);
            if (const auto* parent = registry.try_get<Parent>(e);
                parent != nullptr && parent->entity != entt::null &&
                registry.valid(parent->entity)) {
                if (const auto* pt = registry.try_get<Transform>(parent->entity)) {
                    parent_world = pt->world;
                }
            }
            t.local = glm::inverse(parent_world) * pose;
            t.world = pose;
        }
    }
}

void character_system(entt::registry& registry, physics::PhysicsWorld& world, float dt) {
    const std::uint32_t invalid = physics::PhysicsWorld::invalid_body;
    constexpr float gravity = -9.81F;

    for (auto [e, cc, t] : registry.view<CharacterController, Transform>().each()) {
        if (cc.character == invalid) {
            cc.character = world.create_character(cc.half_height, cc.radius, position_of(t));
        }

        glm::vec3 velocity = cc.velocity;
        if (cc.on_ground && velocity.y < 0.0F) {
            velocity.y = 0.0F; // grounded: stop accumulating downward speed
        }
        velocity.y += gravity * dt;
        velocity.x = cc.move.x * cc.speed;
        velocity.z = cc.move.z * cc.speed;

        world.character_set_velocity(cc.character, velocity);
        world.character_update(cc.character, dt);

        cc.velocity = world.character_velocity(cc.character);
        cc.on_ground = world.character_on_ground(cc.character);
        const glm::vec3 pos = world.character_position(cc.character);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
    }
}

std::expected<void, core::Error> run_character_self_test() {
    auto world = physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());

    // Static floor (top at y=0), added directly to the world.
    physics::PhysicsWorld::BodyParams floor;
    floor.shape = world->create_box(glm::vec3(50.0F, 1.0F, 50.0F));
    floor.position = glm::vec3(0.0F, -1.0F, 0.0F);
    floor.motion = physics::Motion::static_body;
    floor.layer = physics::Layer::static_body;
    (void)world->add_body(floor);

    entt::registry registry;
    const entt::entity e = registry.create();
    registry.emplace<Transform>(e).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 5.0F, 0.0F));
    registry.emplace<CharacterController>(e);

    // Fall onto the floor (4 s).
    for (int i = 0; i < 240; ++i) {
        character_system(registry, *world, 1.0F / 60.0F);
    }
    auto& cc = registry.get<CharacterController>(e);
    const float landed_y = registry.get<Transform>(e).local[3].y;
    if (landed_y > 4.0F) {
        return std::unexpected(core::Error{"character self-test: did not fall"});
    }
    if (!cc.on_ground) {
        return std::unexpected(core::Error{"character self-test: not grounded after landing"});
    }
    if (landed_y < 0.0F) {
        return std::unexpected(core::Error{"character self-test: sank through floor"});
    }

    // Walk +X for 1 s and confirm horizontal motion.
    const float start_x = registry.get<Transform>(e).local[3].x;
    cc.move = glm::vec3(1.0F, 0.0F, 0.0F);
    for (int i = 0; i < 60; ++i) {
        character_system(registry, *world, 1.0F / 60.0F);
    }
    const float end_x = registry.get<Transform>(e).local[3].x;
    if (end_x - start_x < 1.0F) {
        return std::unexpected(core::Error{"character self-test: did not walk forward"});
    }
    return {};
}

std::expected<void, core::Error> run_physics_body_self_test() {
    auto world = physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());

    entt::registry registry;

    // Bodies are created from Transform.world (entities may be parented); this
    // bare-registry test has no transform system, so world is set explicitly.
    const auto place = [&](entt::entity e, const glm::vec3& pos) -> Transform& {
        auto& t = registry.emplace<Transform>(e);
        t.local = glm::translate(glm::mat4(1.0F), pos);
        t.world = t.local;
        return t;
    };

    // Static floor: 100x2x100 box centred at y=-1 (top at y=0).
    const entt::entity floor = registry.create();
    place(floor, glm::vec3(0.0F, -1.0F, 0.0F));
    registry.emplace<Collider>(floor, ColliderShape::box, glm::vec3(50.0F, 1.0F, 50.0F),
                               glm::vec3(0.0F));
    registry.emplace<RigidBody>(floor, BodyMotion::static_body, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    // Dynamic sphere (radius 0.5) dropped from y=5.
    const entt::entity sphere = registry.create();
    place(sphere, glm::vec3(0.0F, 5.0F, 0.0F));
    registry.emplace<Collider>(sphere, ColliderShape::sphere, glm::vec3(0.5F), glm::vec3(0.0F));
    registry.emplace<RigidBody>(sphere, BodyMotion::dynamic, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    // Parented dynamic sphere (the glTF-child case): the parent carries the
    // spawn offset, the child's local is identity — the body must be created
    // at the WORLD position and the pulled pose written back through the
    // parent (local = inverse(parent_world) * pose).
    const entt::entity parent = registry.create();
    Transform& parent_t = place(parent, glm::vec3(10.0F, 5.0F, 0.0F));
    const entt::entity child = registry.create();
    auto& child_t = registry.emplace<Transform>(child);
    child_t.local = glm::mat4(1.0F);
    child_t.world = parent_t.world; // as the transform system would compute
    registry.emplace<Parent>(child, parent);
    registry.emplace<Collider>(child, ColliderShape::sphere, glm::vec3(0.5F), glm::vec3(0.0F));
    registry.emplace<RigidBody>(child, BodyMotion::dynamic, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    // Scaled dynamic sphere (the glTF-with-node-scale case): world scale 2 and
    // authored radius 0.5 must simulate as a radius-1 body — it rests at y=1,
    // matching what the editor's collider wireframe shows.
    const entt::entity scaled = registry.create();
    auto& scaled_t = registry.emplace<Transform>(scaled);
    scaled_t.local = glm::translate(glm::mat4(1.0F), glm::vec3(20.0F, 5.0F, 0.0F)) *
                     glm::scale(glm::mat4(1.0F), glm::vec3(2.0F));
    scaled_t.world = scaled_t.local;
    registry.emplace<Collider>(scaled, ColliderShape::sphere, glm::vec3(0.5F), glm::vec3(0.0F));
    registry.emplace<RigidBody>(scaled, BodyMotion::dynamic, 1.0F,
                                physics::PhysicsWorld::invalid_body);

    for (int i = 0; i < 120; ++i) {
        physics_system(registry, *world, 1.0F / 60.0F);
    }

    const float scaled_y = registry.get<Transform>(scaled).world[3].y;
    if (std::abs(scaled_y - 1.0F) > 0.25F) {
        return std::unexpected(
            core::Error{"physics body self-test: world scale not applied to collider shape"});
    }

    const float y = registry.get<Transform>(sphere).local[3].y;
    if (y > 4.5F) {
        return std::unexpected(core::Error{"physics body self-test: sphere did not fall"});
    }
    if (y < 0.0F) {
        return std::unexpected(core::Error{"physics body self-test: sphere fell through floor"});
    }
    if (std::abs(y - 0.5F) > 0.25F) {
        return std::unexpected(core::Error{"physics body self-test: sphere not resting on floor"});
    }

    // The child body fell at x=10 (its world position, not its identity local).
    const glm::vec3 child_world = glm::vec3(registry.get<Transform>(child).world[3]);
    if (std::abs(child_world.x - 10.0F) > 0.5F || std::abs(child_world.y - 0.5F) > 0.25F) {
        return std::unexpected(
            core::Error{"physics body self-test: parented body not simulated in world space"});
    }
    // And its local pose composes through the parent to reproduce that world.
    const glm::mat4 recomposed = parent_t.world * registry.get<Transform>(child).local;
    if (std::abs(recomposed[3].y - child_world.y) > 0.01F) {
        return std::unexpected(
            core::Error{"physics body self-test: parented local/world out of sync"});
    }
    return {};
}

std::uint32_t add_terrain_body(entt::registry& registry, physics::PhysicsWorld& world,
                               const terrain::Heightmap& map) {
    if (!map.valid()) return physics::PhysicsWorld::invalid_body;
    const auto view = registry.view<const Terrain>();
    if (view.begin() == view.end()) return physics::PhysicsWorld::invalid_body;
    const entt::entity e = *view.begin();
    const auto* tf = registry.try_get<Transform>(e);
    const glm::vec3 pos = tf != nullptr ? glm::vec3(tf->world[3]) : glm::vec3(0.0F);
    const float tile = map.tile_size_m;
    const glm::vec3 origin(pos.x - tile * 0.5F, pos.y, pos.z - tile * 0.5F);

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
    return world.add_body(params);
}

std::expected<void, core::Error> run_terrain_physics_self_test() {
    terrain::TerrainParams tp;
    tp.resolution = 65;
    tp.tile_size_m = 64.0F;
    tp.height_m = 8.0F;
    tp.octaves = 4;
    tp.erosion_droplets = 0;
    const terrain::Heightmap map = terrain::generate(tp);

    auto world = physics::PhysicsWorld::create();
    if (!world) return std::unexpected(world.error());

    entt::registry registry;
    const entt::entity te = registry.create();
    registry.emplace<Terrain>(te, Terrain{tp});
    auto& tf = registry.emplace<Transform>(te);
    tf.local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 2.0F, -5.0F));
    tf.world = tf.local;

    if (add_terrain_body(registry, *world, map) == physics::PhysicsWorld::invalid_body) {
        return std::unexpected(core::Error{"terrain physics self-test: body creation failed"});
    }

    // Drop a sphere onto the tile centre through the ECS physics system.
    constexpr float radius = 0.5F;
    const entt::entity sphere = registry.create();
    auto& st = registry.emplace<Transform>(sphere);
    st.local = glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 40.0F, -5.0F));
    st.world = st.local;
    Collider col;
    col.shape = ColliderShape::sphere;
    col.half_extents = glm::vec3(radius);
    registry.emplace<Collider>(sphere, col);
    RigidBody rb;
    rb.motion = BodyMotion::dynamic;
    registry.emplace<RigidBody>(sphere, rb);

    for (int i = 0; i < 300; ++i) {
        physics_system(registry, *world, 1.0F / 60.0F);
    }

    // Expected rest height: world terrain surface at the drop point + radius.
    // The tile centre sits at the entity's translation, so the drop at
    // (10, -5) lands at heightmap centre; entity y (2) offsets the surface.
    const float surface = 2.0F + map.sample(map.tile_size_m * 0.5F, map.tile_size_m * 0.5F);
    const float y = registry.get<Transform>(sphere).world[3].y;
    if (y > 39.0F) {
        return std::unexpected(core::Error{"terrain physics self-test: sphere did not fall"});
    }
    if (std::abs(y - (surface + radius)) > 1.0F) {
        return std::unexpected(
            core::Error{"terrain physics self-test: sphere not resting on the terrain surface"});
    }
    return {};
}

} // namespace engine::scene
