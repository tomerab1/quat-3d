#include "app/demo_scenes.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/core/env.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/primitives.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

using engine::scene::make_box_data;
using engine::scene::make_cube_data;
using engine::scene::make_sphere_data;

std::expected<DemoScene, engine::core::Error>
build_demo_scene(engine::scene::Scene& scene, engine::asset::AssetManager& assets,
                 engine::rhi::GpuAllocator& allocator,
                 const engine::rhi::TransferContext& setup_transfer) {
    entt::entity spin_entity = entt::null;
    entt::entity demo_root = entt::null;
    entt::entity pedestal = entt::null;
    glm::vec2 footprint_half{0.0F};
    glm::vec3 spin_pos{0.0F};
    float spin_scale = 1.0F;
    std::optional<engine::physics::PhysicsWorld> physics_world;
    bool physics_mode = false;
    bool character_mode = false;
    entt::entity character_entity = entt::null;
    bool scene_ok = true;

    if (const auto gltf_path = engine::core::env_path("QUAT_GLTF")) {
        auto roots = engine::scene::GltfLoader::instantiate(*gltf_path, allocator, setup_transfer,
                                                            assets, scene);
        if (!roots) {
            std::fprintf(stderr, "[fatal] glTF instantiate failed: %s\n",
                         roots.error().message.c_str());
            scene_ok = false;
        } else {
            std::fprintf(stderr, "[info] loaded '%s': %zu root node(s), %zu entities\n",
                         engine::core::path_to_utf8(*gltf_path).c_str(),
                         roots->size(),
                         static_cast<std::size_t>(
                             scene.registry().view<engine::scene::Transform>().size()));
        }
    } else if (std::getenv("QUAT_PHYSICS") != nullptr) {
        // Physics demo: a pile of boxes dropped onto a static floor.
        physics_mode = true;
        using engine::scene::BodyMotion;
        using engine::scene::Collider;
        using engine::scene::ColliderShape;
        using engine::scene::MeshRenderer;
        using engine::scene::RigidBody;
        using engine::scene::Transform;
        constexpr std::uint32_t no_body = engine::physics::PhysicsWorld::invalid_body;

        auto world = engine::physics::PhysicsWorld::create();
        auto floor_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/floor", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(10.0F, 0.5F, 10.0F)),
                                                  allocator, setup_transfer);
            });
        auto box_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/box", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.5F)), allocator,
                                                  setup_transfer);
            });
        auto floor_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/floor_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.5F, 0.5F, 0.55F, 1.0F};
                p.metallic_factor = 0.0F;
                p.roughness_factor = 0.9F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });
        auto box_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/box_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.85F, 0.4F, 0.2F, 1.0F};
                p.metallic_factor = 0.0F;
                p.roughness_factor = 0.5F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });

        if (!world || !floor_mesh.is_loaded() || !box_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            physics_world = std::move(*world);

            const entt::entity floor = scene.create_entity("floor");
            scene.registry().get<Transform>(floor).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.5F, 0.0F));
            scene.registry().emplace<MeshRenderer>(floor, floor_mesh, floor_mat);
            scene.registry().emplace<Collider>(floor, ColliderShape::box,
                                               glm::vec3(10.0F, 0.5F, 10.0F), glm::vec3(0.0F));
            scene.registry().emplace<RigidBody>(floor, BodyMotion::static_body, 1.0F, no_body);

            // A stack of boxes, jittered into 3x3 columns climbing upward.
            for (int i = 0; i < 12; ++i) {
                const float fx = static_cast<float>((i % 3) - 1) * 0.6F;
                const float fz = static_cast<float>((i / 3) % 3 - 1) * 0.6F;
                const float fy = 3.0F + static_cast<float>(i) * 1.15F;
                const entt::entity b = scene.create_entity("box");
                scene.registry().get<Transform>(b).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(fx, fy, fz)) *
                    glm::rotate(glm::mat4(1.0F), 0.3F * static_cast<float>(i),
                                glm::normalize(glm::vec3(1.0F, 0.6F, 0.3F)));
                scene.registry().emplace<MeshRenderer>(b, box_mesh, box_mat);
                scene.registry().emplace<Collider>(b, ColliderShape::box, glm::vec3(0.5F),
                                                   glm::vec3(0.0F));
                scene.registry().emplace<RigidBody>(b, BodyMotion::dynamic, 1.0F, no_body);
            }

            // A transparent glass panel in front of the pile: a static body
            // matching its mesh, so boxes bounce off it (issue #46).
            const glm::vec3 glass_half(3.0F, 2.2F, 0.08F);
            auto glass_mesh = assets.load<engine::asset::MeshAsset>(
                "demo/glass", [&](const std::filesystem::path&) {
                    return engine::scene::upload_mesh(make_box_data(glass_half), allocator,
                                                      setup_transfer);
                });
            auto glass_mat = assets.load<engine::asset::MaterialAsset>(
                "demo/glass_mat", [&](const std::filesystem::path&) {
                    engine::asset::PbrMaterialParams p;
                    p.base_color_factor = {0.75F, 0.88F, 1.0F, 1.0F}; // light blue tint
                    p.transmission_factor = 1.0F;
                    p.metallic_factor = 0.0F;
                    p.roughness_factor = 0.05F; // clear glass
                    p.flags = engine::asset::material_transmission;
                    return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                          allocator, setup_transfer);
                });
            if (glass_mesh.is_loaded()) {
                const entt::entity glass = scene.create_entity("glass");
                scene.registry().get<Transform>(glass).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.2F, 3.0F));
                scene.registry().emplace<MeshRenderer>(glass, glass_mesh, glass_mat);
                scene.registry().emplace<Collider>(glass, ColliderShape::box, glass_half,
                                                   glm::vec3(0.0F));
                scene.registry().emplace<RigidBody>(glass, BodyMotion::static_body, 1.0F, no_body);
            }
        }
    } else if (std::getenv("QUAT_CHARACTER") != nullptr) {
        // Character demo: walk a capsule around a floor with WASD (third-person).
        character_mode = true;
        using engine::scene::Camera;
        using engine::scene::CharacterController;
        using engine::scene::MeshRenderer;
        using engine::scene::Transform;

        auto world = engine::physics::PhysicsWorld::create();
        auto floor_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/cfloor", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(10.0F, 0.5F, 10.0F)),
                                                  allocator, setup_transfer);
            });
        // Capsule stand-in: a box sized to the capsule's bounding volume.
        auto body_mesh = assets.load<engine::asset::MeshAsset>(
            "demo/player", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.3F, 0.9F, 0.3F)),
                                                  allocator, setup_transfer);
            });
        auto floor_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/cfloor_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.5F, 0.5F, 0.55F, 1.0F};
                p.roughness_factor = 0.9F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });
        auto body_mat = assets.load<engine::asset::MaterialAsset>(
            "demo/player_mat", [&](const std::filesystem::path&) {
                engine::asset::PbrMaterialParams p;
                p.base_color_factor = {0.25F, 0.5F, 0.85F, 1.0F};
                p.roughness_factor = 0.4F;
                return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                      allocator, setup_transfer);
            });

        if (!world || !floor_mesh.is_loaded() || !body_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            // Static floor body (collision) added directly to the world.
            engine::physics::PhysicsWorld::BodyParams floor_body;
            floor_body.shape = world->create_box(glm::vec3(10.0F, 0.5F, 10.0F));
            floor_body.position = glm::vec3(0.0F, -0.5F, 0.0F);
            floor_body.motion = engine::physics::Motion::static_body;
            floor_body.layer = engine::physics::Layer::static_body;
            (void)world->add_body(floor_body);
            physics_world = std::move(*world);

            const entt::entity floor = scene.create_entity("floor");
            scene.registry().get<Transform>(floor).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -0.5F, 0.0F));
            scene.registry().emplace<MeshRenderer>(floor, floor_mesh, floor_mat);

            character_entity = scene.create_entity("player");
            scene.registry().get<Transform>(character_entity).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, 2.0F, 0.0F));
            scene.registry().emplace<MeshRenderer>(character_entity, body_mesh, body_mat);
            scene.registry().emplace<CharacterController>(character_entity);
        }
    } else {
        // Showcase scene: a ground plane under the sky, a row of PBR spheres
        // sweeping roughness (top, metallic) and another sweeping metalness
        // (bottom, dielectric), a warm emissive cube, and a clear-glass sphere
        // that refracts the scene behind it. Sun + sky + point lights added below.
        using engine::scene::MeshRenderer;
        using engine::scene::Transform;
        auto make_mat = [&](const std::string& id, glm::vec4 color, float metal, float rough,
                            glm::vec3 emissive = glm::vec3(0.0F), std::uint32_t flags = 0) {
            return assets.load<engine::asset::MaterialAsset>(
                id, [&](const std::filesystem::path&) {
                    engine::asset::PbrMaterialParams p;
                    p.base_color_factor = color;
                    p.metallic_factor = metal;
                    p.roughness_factor = rough;
                    p.emissive_factor = glm::vec4(emissive, 0.0F);
                    if ((flags & engine::asset::material_transmission) != 0) {
                        p.transmission_factor = 1.0F;
                    }
                    p.flags = flags;
                    return engine::asset::upload_material(p, engine::asset::MaterialTextures{},
                                                          allocator, setup_transfer);
                });
        };

        const glm::vec2 k_plate_half(14.0F, 14.0F);
        auto ground_mesh = assets.load<engine::asset::MeshAsset>(
            "show/ground", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(
                    make_box_data(glm::vec3(k_plate_half.x, k_pedestal_mesh_half_height,
                                            k_plate_half.y)),
                    allocator, setup_transfer);
            });
        auto sphere_mesh = assets.load<engine::asset::MeshAsset>(
            "show/sphere", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_sphere_data(0.55F, 32, 48), allocator,
                                                  setup_transfer);
            });
        auto cube_mesh = assets.load<engine::asset::MeshAsset>(
            "show/cube", [&](const std::filesystem::path&) {
                return engine::scene::upload_mesh(make_box_data(glm::vec3(0.5F)), allocator,
                                                  setup_transfer);
            });
        auto ground_mat = make_mat("show/ground_mat", {0.42F, 0.45F, 0.5F, 1.0F}, 0.0F, 0.85F);

        if (!ground_mesh.is_loaded() || !sphere_mesh.is_loaded() || !cube_mesh.is_loaded()) {
            scene_ok = false;
        } else {
            // MeshSource tags carry the cache keys so scene save/load can
            // re-resolve these code-created assets (recreated every startup).
            using engine::scene::MeshSource;
            // Everything hangs under one root so the showcase moves as a
            // whole (QUAT_TERRAIN settles it onto the generated ground).
            demo_root = scene.create_entity("showcase");
            const auto add = [&](const char* name) {
                const entt::entity e = scene.create_entity(name);
                scene.set_parent(e, demo_root);
                return e;
            };
            const entt::entity ground = add("ground");
            pedestal = ground;
            footprint_half = k_plate_half;
            scene.registry().get<Transform>(ground).local = glm::translate(
                glm::mat4(1.0F), glm::vec3(0.0F, -k_pedestal_mesh_half_height, 0.0F));
            scene.registry().emplace<MeshRenderer>(ground, ground_mesh, ground_mat);
            scene.registry().emplace<MeshSource>(ground, "show/ground", "show/ground_mat", "");

            constexpr int count = 6;
            for (int i = 0; i < count; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(count - 1);
                const float x = (t - 0.5F) * 7.5F;
                // Top row: polished metal, roughness sweep.
                const entt::entity m = add("metal_sphere");
                scene.registry().get<Transform>(m).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.55F, -1.6F));
                scene.registry().emplace<MeshRenderer>(
                    m, sphere_mesh,
                    make_mat("show/metal" + std::to_string(i), {0.95F, 0.78F, 0.45F, 1.0F}, 1.0F,
                             glm::clamp(t, 0.05F, 0.95F)));
                scene.registry().emplace<MeshSource>(m, "show/sphere",
                                                     "show/metal" + std::to_string(i), "");
                // Bottom row: coloured dielectric, metalness sweep.
                const entt::entity d_ent = add("dielectric_sphere");
                scene.registry().get<Transform>(d_ent).local =
                    glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.55F, 1.4F));
                scene.registry().emplace<MeshRenderer>(
                    d_ent, sphere_mesh,
                    make_mat("show/diel" + std::to_string(i), {0.2F, 0.45F, 0.85F, 1.0F},
                             glm::clamp(t, 0.0F, 1.0F), 0.35F));
                scene.registry().emplace<MeshSource>(d_ent, "show/sphere",
                                                     "show/diel" + std::to_string(i), "");
            }

            // A warm emissive cube as a visible local light source.
            const entt::entity glow = add("glow_cube");
            scene.registry().get<Transform>(glow).local =
                glm::translate(glm::mat4(1.0F), glm::vec3(-5.5F, 0.5F, 4.0F)) *
                glm::rotate(glm::mat4(1.0F), 0.6F, glm::vec3(0.2F, 1.0F, 0.1F));
            scene.registry().emplace<MeshRenderer>(
                glow, cube_mesh,
                make_mat("show/glow_mat", {0.0F, 0.0F, 0.0F, 1.0F}, 0.0F, 1.0F,
                         glm::vec3(3.0F, 1.2F, 0.3F)));
            scene.registry().emplace<MeshSource>(glow, "show/cube", "show/glow_mat", "");

            // A clear glass sphere up front that refracts the spheres behind it.
            const entt::entity glass = add("glass_sphere");
            spin_entity = glass; // gently rotate it so the refraction shifts
            spin_pos = glm::vec3(0.0F, 0.85F, 4.2F);
            spin_scale = 1.5F;
            scene.registry().get<Transform>(glass).local =
                glm::translate(glm::mat4(1.0F), spin_pos) *
                glm::scale(glm::mat4(1.0F), glm::vec3(spin_scale));
            scene.registry().emplace<MeshRenderer>(
                glass, sphere_mesh,
                make_mat("show/glass_mat", {0.85F, 0.92F, 1.0F, 1.0F}, 0.0F, 0.04F,
                         glm::vec3(0.0F), engine::asset::material_transmission));
            scene.registry().emplace<MeshSource>(glass, "show/sphere", "show/glass_mat", "");
        }
    }

    if (!scene_ok) return std::unexpected(engine::core::Error{"demo scene setup failed"});
    DemoScene out;
    out.physics_mode = physics_mode;
    out.character_mode = character_mode;
    out.from_gltf = engine::core::env_path("QUAT_GLTF").has_value();
    out.physics_world = std::move(physics_world);
    out.character_entity = character_entity;
    out.spin_entity = spin_entity;
    out.spin_pos = spin_pos;
    out.spin_scale = spin_scale;
    out.root = demo_root;
    out.pedestal = pedestal;
    out.footprint_half = footprint_half;
    return out;
}

void add_default_lighting(engine::scene::Scene& scene, bool fill_lights, entt::entity root) {
// A sun light, and a camera that auto-frames whatever was loaded.
const entt::entity sun_entity = scene.create_entity("sun");
scene.registry().emplace<engine::scene::DirectionalLight>(
    sun_entity, glm::normalize(glm::vec3(-0.4F, -1.0F, -0.3F)),
    glm::vec3(1.0F, 0.98F, 0.95F), 3.0F);

// Two coloured point lights for warmer/cooler fill (skipped for bare glTF
// loads so a model is shown under just its own lighting setup).
if (fill_lights) {
    const entt::entity warm = scene.create_entity("point_warm");
    scene.registry().get<engine::scene::Transform>(warm).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(-3.5F, 3.0F, 2.5F));
    scene.registry().emplace<engine::scene::PointLight>(warm, glm::vec3(1.0F, 0.55F, 0.2F),
                                                        14.0F, 12.0F);
    const entt::entity cool = scene.create_entity("point_cool");
    scene.registry().get<engine::scene::Transform>(cool).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(3.5F, 3.0F, -2.5F));
    scene.registry().emplace<engine::scene::PointLight>(cool, glm::vec3(0.3F, 0.5F, 1.0F),
                                                        14.0F, 12.0F);
    if (root != entt::null) {
        scene.set_parent(warm, root);
        scene.set_parent(cool, root);
    }
}

}

void add_env_terrain(engine::scene::Scene& scene) {
if (std::getenv("QUAT_TERRAIN") != nullptr) {
    const entt::entity te = scene.create_entity("terrain");
    auto& t = scene.registry().emplace<engine::scene::Terrain>(te);
    t.params.resolution = 513;
    t.params.tile_size_m = 800.0F;
    t.params.height_m = 90.0F;
    t.snowline_m = 30.0F;
    scene.registry().get<engine::scene::Transform>(te).local =
        glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -40.0F, 0.0F));
    auto& grass = scene.registry().emplace<engine::scene::Grass>(scene.create_entity("grass"));
    grass.settings.height_m = glm::vec2(-10000.0F, t.snowline_m - 5.0F); // none on the snow
    std::fprintf(stderr, "[terrain] QUAT_TERRAIN tile requested\n");
}
}

} // namespace quat::app
