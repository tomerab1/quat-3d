// Scene files re-attach each skinned primitive's skin by the glTF node that
// instanced it (S1 audit C12): one mesh instanced by two nodes with
// different skins used to get the first node's skin on both after a load.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>
#include <gtest/gtest.h>

#include "engine/asset/asset_manager.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace s = engine::scene;

std::string base64(const std::byte* data, std::size_t size) {
    static constexpr char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    const auto at = [&](std::size_t k) {
        return k < size ? static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[k])) : 0U;
    };
    for (std::size_t i = 0; i < size; i += 3) {
        const std::uint32_t n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += i + 1 < size ? tbl[(n >> 6) & 63] : '=';
        out += i + 2 < size ? tbl[n & 63] : '=';
    }
    return out;
}

// One skinned triangle (every vertex 100% joint 0), instanced by node 0
// ("left", skin 0, joint "bone_a") and node 1 ("right", skin 1, joint
// "bone_b", placed elsewhere so its armature offset differs too).
std::string two_skins_one_mesh() {
    constexpr std::size_t kPos = 36, kJoints = 12, kWeights = 48;
    std::array<std::byte, kPos + kJoints + kWeights> buf{};
    const std::array<float, 9> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::array<std::uint8_t, 12> joints{};
    const std::array<float, 12> weights{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(buf.data(), pos.data(), kPos);
    std::memcpy(buf.data() + kPos, joints.data(), kJoints);
    std::memcpy(buf.data() + kPos + kJoints, weights.data(), kWeights);
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1,2,3]}],)"
           R"("nodes":[{"name":"left","mesh":0,"skin":0},)"
           R"({"name":"right","mesh":0,"skin":1,"translation":[5,0,0]},)"
           R"({"name":"bone_a"},{"name":"bone_b","translation":[0,2,0]}],)"
           R"("skins":[{"joints":[2]},{"joints":[3]}],)"
           R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]}],)"
           R"("buffers":[{"byteLength":96,"uri":"data:application/octet-stream;base64,)" +
           base64(buf.data(), buf.size()) +
           R"("}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
           R"({"buffer":0,"byteOffset":36,"byteLength":12},)"
           R"({"buffer":0,"byteOffset":48,"byteLength":48}],)"
           R"("accessors":[)"
           R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
           R"({"bufferView":1,"componentType":5121,"count":3,"type":"VEC4"},)"
           R"({"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"}]})";
}

struct SkinSeen {
    std::string joint;      // the skeleton's (only) joint name
    glm::mat4   root{1.0F}; // SkinnedMesh::root_transform
};

// Name -> skin of every skinned entity in `scene`.
std::map<std::string, SkinSeen> skins_by_name(s::Scene& scene) {
    std::map<std::string, SkinSeen> out;
    for (auto [e, name, skinned] : scene.registry().view<s::Name, s::SkinnedMesh>().each()) {
        SkinSeen seen;
        if (skinned.skeleton.is_loaded() && !skinned.skeleton->joints.empty()) {
            seen.joint = skinned.skeleton->joints.front().name;
        }
        seen.root = skinned.root_transform;
        out[name.value] = seen;
    }
    return out;
}

TEST(GpuSceneIo, InstancedSkinnedMeshKeepsEachNodesSkin) {
    QUAT_REQUIRE_GPU(g);
    const auto dir = std::filesystem::temp_directory_path() / "quat_scene_io_skin";
    std::filesystem::create_directories(dir);
    const auto gltf = dir / "two_skins.gltf";
    std::ofstream(gltf) << two_skins_one_mesh();

    engine::asset::AssetManager assets;
    s::Scene scene;
    ASSERT_OK(s::GltfLoader::instantiate(gltf, g->allocator, g->transfer, assets, scene));
    const auto before = skins_by_name(scene);
    ASSERT_EQ(before.size(), 2U);
    ASSERT_EQ(before.at("left").joint, "bone_a");
    ASSERT_EQ(before.at("right").joint, "bone_b");

    const auto file = dir / "scene.json";
    ASSERT_OK(s::save_scene(scene, file));
    ASSERT_OK(s::load_scene(scene, file, g->allocator, g->transfer, assets));

    const auto after = skins_by_name(scene);
    ASSERT_EQ(after.size(), 2U);
    for (const auto& [name, seen] : before) {
        ASSERT_TRUE(after.contains(name)) << name;
        EXPECT_EQ(after.at(name).joint, seen.joint) << name << " got another node's skin";
        EXPECT_EQ(after.at(name).root, seen.root) << name << " got another node's armature offset";
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

} // namespace
