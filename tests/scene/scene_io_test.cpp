// Scene files (save_scene / load_scene). load_scene takes GPU upload handles
// (glTF meshes re-resolve through them), so these run in a Gpu* suite.

#include <filesystem>
#include <fstream>
#include <string>

#include <entt/entity/registry.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
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

} // namespace
