// Scene files (save_scene / load_scene). load_scene takes GPU upload handles
// (glTF meshes re-resolve through them), so these run in a Gpu* suite.

#include <filesystem>
#include <fstream>
#include <system_error>
#include <iterator>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/asset_manager.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/scene/component_json.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/scene/physics_settings.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;

// Typed null: GoogleTest can print an entt::entity but not entt::null_t.
constexpr entt::entity k_none = entt::null;

std::filesystem::path write_temp(const std::string& name, const std::string& text) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << text;
    return path;
}

entt::entity find_named(s::Scene& scene, const std::string& name) {
    for (const auto [e, n] : scene.registry().view<s::Name>().each()) {
        if (n.value == name) return e;
    }
    return entt::null;
}

TEST(GpuSceneIo, MalformedFilesFailAndKeepTheLiveScene) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    scene.create_entity("keep_me");

    const std::string bad_files[] = {
        "not json",
        R"({"entities": 5})",
        R"({"entities": [{"name": 7}]})",
        R"({"entities": [{"parent": "zero"}]})",
        R"({"entities": [{"parent": 0}]})",                              // self-parent
        R"({"entities": [{"parent": 1}, {"parent": 0}]})",               // cycle
        R"({"entities": [{"point_light": {"radius": "far"}}]})",
        R"({"entities": [{"collider": {"half_extents": [1, 1]}}]})",
        R"({"entities": [{"transform": [1, 2, 3]}]})",
        R"({"entities": [{"mesh": {"mesh_key": 3}}]})",
    };
    for (std::size_t i = 0; i < std::size(bad_files); ++i) {
        const auto path = write_temp("quat_bad_scene_" + std::to_string(i) + ".json", bad_files[i]);
        EXPECT_ERR(s::load_scene(scene, path, g->allocator, g->transfer, assets))
            << "file: " << bad_files[i];
        std::filesystem::remove(path);
        EXPECT_NE(find_named(scene, "keep_me"), k_none) << "live scene lost after: " << bad_files[i];
    }
}

TEST(GpuSceneIo, LoadsVersion1FilesWithIntegerEnums) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const auto path = write_temp("quat_v1_scene.json", R"({"version": 1, "entities": [
        {"name": "box", "parent": -1,
         "collider": {"shape": 2, "half_extents": [0.4, 0.9, 0.4], "offset": [0, 0, 0], "sensor": false},
         "rigidbody": {"motion": 1, "mass": 3},
         "camera": {"fov_y": 1.0, "near": 0.1, "far": 100, "active": true}}]})");
    ASSERT_OK(s::load_scene(scene, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);
    const entt::entity e = find_named(scene, "box");
    ASSERT_NE(e, k_none);
    EXPECT_EQ(scene.registry().get<s::Collider>(e).shape, s::ColliderShape::capsule);
    EXPECT_EQ(scene.registry().get<s::RigidBody>(e).motion, s::BodyMotion::kinematic);
    EXPECT_NEAR(scene.registry().get<s::Camera>(e).fov_y, 1.0F, 1e-5F);
}

TEST(GpuSceneIo, RoundTripsComponentsVersion1DidNotSave) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const entt::entity e = scene.create_entity("player");
    scene.registry().emplace<s::CharacterController>(e).speed = 7.5F;
    scene.registry().emplace<s::NetReplicated>(e, 99U);
    auto& prim = scene.registry().emplace<s::Primitive>(e);
    prim.shape = s::PrimitiveShape::capsule;
    const entt::entity child = scene.create_entity("child");
    ASSERT_TRUE(scene.set_parent(child, e));

    const auto path = std::filesystem::temp_directory_path() / "quat_roundtrip_scene.json";
    ASSERT_OK(s::save_scene(scene, path));
    s::Scene loaded;
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);

    const entt::entity le = find_named(loaded, "player");
    const entt::entity lc = find_named(loaded, "child");
    ASSERT_NE(le, k_none);
    ASSERT_NE(lc, k_none);
    auto& r = loaded.registry();
    EXPECT_FLOAT_EQ(r.get<s::CharacterController>(le).speed, 7.5F);
    EXPECT_EQ(r.get<s::NetReplicated>(le).net_id, 99U);
    EXPECT_EQ(r.get<s::Primitive>(le).shape, s::PrimitiveShape::capsule);
    EXPECT_EQ(r.get<s::Parent>(lc).entity, le);
}

// R10.2 backward compatibility: a terrain saved before the erosion model
// existed (no "erosion" / "gpu" keys) loads as the droplet-eroded CPU
// terrain it was — not as a new component's GPU grid default — and keeps
// that through a save/load round trip.
TEST(GpuSceneIo, TerrainWithoutErosionKeyLoadsAsDroplets) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const auto path = write_temp("quat_v2_terrain_scene.json", R"({"version": 2, "entities": [
        {"name": "terrain", "parent": -1,
         "terrain": {"seed": 42, "resolution": 257, "tile_size": 500, "height": 60,
                     "octaves": 5, "warp": 0.2, "droplets": 30000, "snowline": 40,
                     "streaming": false, "stream_radius": 1}}]})");
    ASSERT_OK(s::load_scene(scene, path, g->allocator, g->transfer, assets));
    const entt::entity te = find_named(scene, "terrain");
    ASSERT_NE(te, k_none);
    const s::Terrain& t = scene.registry().get<s::Terrain>(te);
    EXPECT_EQ(t.params.erosion_model, engine::terrain::ErosionModel::droplets);
    EXPECT_EQ(t.params.erosion_droplets, 30000U);
    EXPECT_FALSE(t.gpu_generate);

    ASSERT_OK(s::save_scene(scene, path));
    s::Scene reloaded;
    ASSERT_OK(s::load_scene(reloaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);
    const entt::entity re = find_named(reloaded, "terrain");
    ASSERT_NE(re, k_none);
    const s::Terrain& rt = reloaded.registry().get<s::Terrain>(re);
    EXPECT_EQ(rt.params.erosion_model, engine::terrain::ErosionModel::droplets);
    EXPECT_EQ(rt.params.erosion_droplets, 30000U);
    EXPECT_FALSE(rt.gpu_generate);

    // A newly created terrain still gets the GPU grid default.
    const s::Terrain fresh{};
    EXPECT_EQ(fresh.params.erosion_model, engine::terrain::ErosionModel::grid);
    EXPECT_TRUE(fresh.gpu_generate);
}

// R9.2: authored placements are saved; a scatter saves its parameters only
// (its placements regenerate from them).
TEST(GpuSceneIo, RoundTripsInstancedMeshAndScatter) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const entt::entity rocks = scene.create_entity("rocks");
    auto& mesh = scene.registry().emplace<s::InstancedMesh>(rocks);
    mesh.primitive.shape = s::PrimitiveShape::sphere;
    mesh.cast_shadows = false;
    mesh.shadow_distance = 45.0F;
    for (int i = 0; i < 3; ++i) {
        s::InstanceTransform t;
        t.position = glm::vec3(static_cast<float>(i), 0.5F, -2.0F);
        t.scale = glm::vec3(0.5F + static_cast<float>(i));
        mesh.instances.push_back(t);
    }
    const entt::entity forest = scene.create_entity("forest");
    scene.registry().emplace<s::InstancedMesh>(forest).gltf_path = "assets/meshes/tree.glb";
    auto& scatter = scene.registry().emplace<s::Scatter>(forest);
    scatter.seed = 1234;
    scatter.area = s::ScatterArea::circle;
    scatter.slope_deg = {0.0F, 25.0F};
    scene.registry().get<s::InstancedMesh>(forest).instances.resize(500); // generated

    const auto path = std::filesystem::temp_directory_path() / "quat_instanced_scene.json";
    ASSERT_OK(s::save_scene(scene, path));
    s::Scene loaded;
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);

    auto& r = loaded.registry();
    const entt::entity lr = find_named(loaded, "rocks");
    const entt::entity lf = find_named(loaded, "forest");
    ASSERT_NE(lr, k_none);
    ASSERT_NE(lf, k_none);
    const auto& lmesh = r.get<s::InstancedMesh>(lr);
    EXPECT_EQ(lmesh.primitive.shape, s::PrimitiveShape::sphere);
    EXPECT_FALSE(lmesh.cast_shadows);
    EXPECT_EQ(lmesh.shadow_distance, 45.0F);
    ASSERT_EQ(lmesh.instances.size(), 3U);
    EXPECT_EQ(lmesh.instances[2].position, glm::vec3(2.0F, 0.5F, -2.0F));
    EXPECT_FLOAT_EQ(lmesh.instances[2].scale.x, 2.5F);
    EXPECT_EQ(r.get<s::InstancedMesh>(lf).gltf_path, "assets/meshes/tree.glb");
    EXPECT_TRUE(r.get<s::InstancedMesh>(lf).instances.empty()); // regenerated, not stored
    EXPECT_EQ(r.get<s::Scatter>(lf).seed, 1234U);
    EXPECT_EQ(r.get<s::Scatter>(lf).area, s::ScatterArea::circle);
    EXPECT_EQ(r.get<s::Scatter>(lf).slope_deg, glm::vec2(0.0F, 25.0F));
    EXPECT_EQ(r.get<s::Scatter>(lf).generated, 0U);
}

// A8: the renderer's settings and per-entity material overrides travel with
// the scene; files without the block load with default settings, and a
// malformed block fails the load without touching the live scene or settings.
TEST(GpuSceneIo, RenderSettingsAndMaterialOverridesRoundTrip) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const entt::entity ball = scene.create_entity("ball");
    scene.registry().emplace<s::Primitive>(ball).roughness = 0.2F;
    ASSERT_OK(s::apply_component(scene.registry(), ball, "material",
                                 {{"clearcoat", 1.0}, {"ior", 1.33}}, true));
    engine::renderer::RenderSettings saved;
    saved.exposure.compensation_ev = 1.25F;
    saved.bloom.enabled = false;
    saved.shadows.pcf_radius = 3.0F;
    saved.ao.radius = 1.25F;
    saved.clouds.coverage = 0.9F;

    const auto path = std::filesystem::temp_directory_path() / "quat_settings_scene.json";
    ASSERT_OK(s::save_scene(scene, path, &saved));
    s::Scene loaded;
    engine::renderer::RenderSettings settings;
    settings.taa.feedback = 0.5F;  // the file has the default: restored
    settings.clouds.time_s = 7.0F; // frame-loop state is kept
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets, &settings));
    EXPECT_FLOAT_EQ(settings.exposure.compensation_ev, 1.25F);
    EXPECT_FALSE(settings.bloom.enabled);
    EXPECT_FLOAT_EQ(settings.shadows.pcf_radius, 3.0F);
    EXPECT_FLOAT_EQ(settings.ao.radius, 1.25F);
    EXPECT_FLOAT_EQ(settings.clouds.coverage, 0.9F);
    EXPECT_FLOAT_EQ(settings.taa.feedback, engine::renderer::TaaSettings{}.feedback);
    EXPECT_FLOAT_EQ(settings.clouds.time_s, 7.0F);
    const entt::entity lb = find_named(loaded, "ball");
    ASSERT_NE(lb, k_none);
    const auto params = s::effective_material_params(loaded.registry(), lb);
    ASSERT_TRUE(params.has_value());
    EXPECT_FLOAT_EQ(params->roughness_factor, 0.2F);
    EXPECT_FLOAT_EQ(params->clearcoat_factor, 1.0F);
    EXPECT_NEAR(params->ior, 1.33F, 1e-6F);

    // Saving without settings writes no block; loading such a (pre-A8) file
    // resets the settings to the defaults.
    ASSERT_OK(s::save_scene(scene, path));
    {
        std::ifstream in(path);
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(text.find("render_settings"), std::string::npos);
    }
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets, &settings));
    EXPECT_FLOAT_EQ(settings.exposure.compensation_ev, 0.0F);
    EXPECT_TRUE(settings.bloom.enabled);
    std::filesystem::remove(path);

    // Lenient decoding: unknown groups / fields ignored, out-of-range values
    // clamped — render settings and material overrides alike.
    const auto loose = write_temp("quat_loose_settings.json", R"({"version": 2, "entities": [
        {"name": "orb", "primitive": {"shape": "sphere"},
         "material": {"clearcoat": 3, "ior": 0.5, "sheen": 1}}],
        "render_settings": {"version": 1, "bloom": {"intensity": 9, "flare": true},
                            "lens": {"dirt": 1}}})");
    ASSERT_OK(s::load_scene(loaded, loose, g->allocator, g->transfer, assets, &settings));
    EXPECT_FLOAT_EQ(settings.bloom.intensity, 1.0F);
    const entt::entity orb = find_named(loaded, "orb");
    ASSERT_NE(orb, k_none);
    const auto orb_params = s::effective_material_params(loaded.registry(), orb);
    ASSERT_TRUE(orb_params.has_value());
    EXPECT_FLOAT_EQ(orb_params->clearcoat_factor, 1.0F); // clamped into [0, 1]
    EXPECT_FLOAT_EQ(orb_params->ior, 1.0F);              // clamped into [1, 3]
    std::filesystem::remove(loose);

    // A block newer than this build reads fails the load with a clear error.
    const auto newer = write_temp("quat_newer_settings.json", R"({"entities": [],
        "render_settings": {"version": 99, "bloom": {"intensity": 0.5}}})");
    auto too_new = s::load_scene(loaded, newer, g->allocator, g->transfer, assets, &settings);
    ASSERT_FALSE(too_new);
    EXPECT_NE(too_new.error().message.find("render_settings version 99 is newer"),
              std::string::npos)
        << too_new.error().message;
    EXPECT_NE(find_named(loaded, "orb"), k_none); // the live scene is kept
    std::filesystem::remove(newer);

    // A malformed block fails the load and changes nothing.
    loaded.create_entity("keep_me");
    settings.exposure.key = 0.3F;
    const auto bad = write_temp("quat_bad_settings.json", R"({"entities": [{"name": "x"}],
        "render_settings": {"taa": {"enabled": "yes"}}})");
    auto result = s::load_scene(loaded, bad, g->allocator, g->transfer, assets, &settings);
    ASSERT_FALSE(result);
    EXPECT_NE(result.error().message.find("render_settings: taa.enabled"), std::string::npos)
        << result.error().message;
    EXPECT_FLOAT_EQ(settings.exposure.key, 0.3F);
    EXPECT_NE(find_named(loaded, "keep_me"), k_none);
    EXPECT_EQ(find_named(loaded, "x"), k_none);
    std::filesystem::remove(bad);
}

// A10.1: the scene's gravity is saved as a versioned physics_settings block
// (saving needs no GPU).
TEST(SceneIo, SavesThePhysicsSettingsBlock) {
    s::Scene scene;
    s::physics_settings_mut(scene.registry()).gravity = glm::vec3(0.0F, -1.62F, 0.5F);
    const auto path = std::filesystem::temp_directory_path() / "quat_physics_settings.json";
    ASSERT_OK(s::save_scene(scene, path));
    nlohmann::json root;
    {
        // Closed before the remove: Windows cannot delete an open file.
        std::ifstream in(path);
        root = nlohmann::json::parse(in, nullptr, false);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    EXPECT_FALSE(ec) << ec.message();
    ASSERT_FALSE(root.is_discarded());
    ASSERT_TRUE(root.contains("physics_settings"));
    EXPECT_EQ(root["physics_settings"]["version"], s::k_physics_settings_version);
    EXPECT_NEAR(root["physics_settings"]["gravity"][1].get<float>(), -1.62F, 1e-6F);
    EXPECT_NEAR(root["physics_settings"]["gravity"][2].get<float>(), 0.5F, 1e-6F);
}

// A joint's target is saved as a file index and re-linked on load.
TEST(GpuSceneIo, JointTargetsRoundTrip) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    const entt::entity frame = scene.create_entity("frame");
    const entt::entity door = scene.create_entity("door");
    auto& j = scene.registry().emplace<s::Joint>(door);
    j.type = s::JointType::hinge;
    j.target = frame;
    j.limits_enabled = true;
    j.limits = glm::vec2(-90.0F, 0.0F);
    scene.registry().emplace<s::Joint>(frame).type = s::JointType::point; // to the world
    const auto path = std::filesystem::temp_directory_path() / "quat_joint_rt.json";
    ASSERT_OK(s::save_scene(scene, path));
    {
        std::ifstream in(path);
        const nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
        ASSERT_FALSE(root.is_discarded());
        const nlohmann::json& list = root["entities"];
        ASSERT_EQ(list.size(), 2U);
        const std::size_t door_index = list[0]["name"] == "door" ? 0 : 1;
        const std::size_t frame_index = 1 - door_index;
        EXPECT_EQ(list[door_index]["joint"]["target"], frame_index) << "stored as an index";
        EXPECT_TRUE(list[frame_index]["joint"]["target"].is_null());
        EXPECT_FALSE(list[door_index]["joint"].contains("active")) << "runtime state saved";
    }
    s::Scene loaded;
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);
    const entt::entity lframe = find_named(loaded, "frame");
    const entt::entity ldoor = find_named(loaded, "door");
    ASSERT_TRUE(loaded.registry().all_of<s::Joint>(ldoor));
    const s::Joint& lj = loaded.registry().get<s::Joint>(ldoor);
    EXPECT_EQ(lj.target, lframe);
    EXPECT_EQ(lj.type, s::JointType::hinge);
    EXPECT_FLOAT_EQ(lj.limits.x, -90.0F);
    EXPECT_EQ(loaded.registry().get<s::Joint>(lframe).target, k_none);

    // A target index out of range fails the load.
    const auto bad = write_temp("quat_joint_bad.json", R"({"entities": [
        {"name": "a", "joint": {"type": "point", "target": 5}}]})");
    EXPECT_ERR(s::load_scene(loaded, bad, g->allocator, g->transfer, assets));
    std::filesystem::remove(bad);
}

TEST(GpuSceneIo, PhysicsSettingsRoundTrip) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    s::physics_settings_mut(scene.registry()).gravity = glm::vec3(1.0F, -3.0F, 0.0F);
    const auto path = std::filesystem::temp_directory_path() / "quat_physics_rt.json";
    ASSERT_OK(s::save_scene(scene, path));
    s::Scene loaded;
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets));
    std::filesystem::remove(path);
    EXPECT_EQ(s::physics_settings(loaded.registry()).gravity, glm::vec3(1.0F, -3.0F, 0.0F));

    // Older files have no block: the default gravity.
    const auto old = write_temp("quat_physics_old.json", R"({"entities": []})");
    ASSERT_OK(s::load_scene(loaded, old, g->allocator, g->transfer, assets));
    EXPECT_EQ(s::physics_settings(loaded.registry()).gravity, glm::vec3(0.0F, -9.81F, 0.0F));
    std::filesystem::remove(old);

    // Newer blocks and bad values fail the load.
    const auto newer = write_temp("quat_physics_new.json", R"({"entities": [],
        "physics_settings": {"version": 99, "gravity": [0, -1, 0]}})");
    EXPECT_ERR(s::load_scene(loaded, newer, g->allocator, g->transfer, assets));
    std::filesystem::remove(newer);
    const auto bad = write_temp("quat_physics_bad.json", R"({"entities": [],
        "physics_settings": {"gravity": [0, -1e6, 0]}})");
    EXPECT_ERR(s::load_scene(loaded, bad, g->allocator, g->transfer, assets));
    std::filesystem::remove(bad);
}

// R21: the HDRI path is scene-level data saved next to the render settings
// (whose `environment` group selects it); scenes without one load with none.
TEST(GpuSceneIo, EnvironmentHdriRoundTrips) {
    QUAT_REQUIRE_GPU(g);
    engine::asset::AssetManager assets;
    s::Scene scene;
    scene.create_entity("anchor");
    engine::renderer::RenderSettings saved;
    saved.environment.source = static_cast<int>(engine::renderer::SkySource::hdri);
    saved.environment.hdri_rotation_deg = 120.0F;
    saved.ssr.quality = static_cast<int>(engine::renderer::SsrQuality::high);
    const s::SceneEnvironment env{"hdri-cache/sky_2k.hdr"};
    const auto path = std::filesystem::temp_directory_path() / "quat_env_scene.json";
    ASSERT_OK(s::save_scene(scene, path, &saved, &env));

    s::Scene loaded;
    engine::renderer::RenderSettings settings;
    s::SceneEnvironment read{"stale.hdr"};
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets, &settings, &read));
    EXPECT_EQ(read.hdri, "hdri-cache/sky_2k.hdr");
    EXPECT_EQ(settings.environment.source, static_cast<int>(engine::renderer::SkySource::hdri));
    EXPECT_FLOAT_EQ(settings.environment.hdri_rotation_deg, 120.0F);
    EXPECT_EQ(settings.ssr.quality, static_cast<int>(engine::renderer::SsrQuality::high));

    ASSERT_OK(s::save_scene(scene, path, &saved));
    ASSERT_OK(s::load_scene(loaded, path, g->allocator, g->transfer, assets, &settings, &read));
    EXPECT_TRUE(read.hdri.empty());

    const auto bad = write_temp("quat_bad_env.json", R"({"entities": [],
        "environment": {"hdri": 5}})");
    read.hdri = "keep.hdr";
    auto result = s::load_scene(loaded, bad, g->allocator, g->transfer, assets, &settings, &read);
    ASSERT_FALSE(result);
    EXPECT_EQ(read.hdri, "keep.hdr");
    std::filesystem::remove(bad);
    std::filesystem::remove(path);
}

} // namespace
