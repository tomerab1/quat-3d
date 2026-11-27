// JOINTS_0 indices are validated against the skin of every node that skins
// the mesh (S1 audit C5): an index past the skin's joint count would read the
// joint-matrix buffer out of bounds in the skinning compute pass.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include <fastgltf/core.hpp>
#include <gtest/gtest.h>

#include "scene/gltf/internal.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace g = engine::scene::gltf_detail;

std::string base64(const std::byte* data, std::size_t size) {
    static constexpr char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    const auto at = [&](std::size_t k) {
        return k < size ? static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[k])) : 0U;
    };
    for (; i < size; i += 3) {
        const std::uint32_t n = (at(i) << 16) | (at(i + 1) << 8) | at(i + 2);
        out += tbl[(n >> 18) & 63];
        out += tbl[(n >> 12) & 63];
        out += i + 1 < size ? tbl[(n >> 6) & 63] : '=';
        out += i + 2 < size ? tbl[n & 63] : '=';
    }
    return out;
}

// A one-triangle mesh whose vertex 0 is influenced by `joint` with `weight`
// (the other vertices by joint 0), skinned by a 2-joint skin on node 0.
// `skinned_node` false leaves the mesh node without a skin.
std::string skinned_triangle(std::uint8_t joint, float weight, bool skinned_node = true) {
    constexpr std::size_t kPos = 36, kJoints = 12, kWeights = 48;
    std::array<std::byte, kPos + kJoints + kWeights> buf{};
    const std::array<float, 9> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::array<std::uint8_t, 12> joints{joint, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const std::array<float, 12> weights{weight, 1.0F - weight, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    std::memcpy(buf.data(), pos.data(), kPos);
    std::memcpy(buf.data() + kPos, joints.data(), kJoints);
    std::memcpy(buf.data() + kPos + kJoints, weights.data(), kWeights);

    const std::string mesh_node = skinned_node ? R"({"mesh":0,"skin":0})" : R"({"mesh":0})";
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0,1]}],)"
           R"("nodes":[)" + mesh_node + R"(,{"name":"root","children":[2]},{"name":"bone"}],)"
           R"("skins":[{"joints":[1,2]}],)"
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

// Parses + structurally validates `json`; asserts both succeed.
void parse(const std::string& json, std::optional<fastgltf::Asset>& out) {
    auto data = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(json.data()),
                                                    json.size());
    ASSERT_EQ(data.error(), fastgltf::Error::None);
    fastgltf::Parser parser(g::kParserExtensions);
    auto parsed = parser.loadGltf(data.get(), ".", g::kLoadOptions);
    ASSERT_EQ(parsed.error(), fastgltf::Error::None)
        << fastgltf::getErrorMessage(parsed.error());
    ASSERT_OK(g::validate_asset(parsed.get()));
    out.emplace(std::move(parsed.get()));
}

TEST(GltfSkinBinding, JointsWithinTheSkinLoad) {
    std::optional<fastgltf::Asset> a;
    ASSERT_NO_FATAL_FAILURE(parse(skinned_triangle(1, 0.5F), a));
    EXPECT_EQ(g::max_weighted_joint(*a, a->meshes[0].primitives[0]), 1U);
    EXPECT_OK(g::validate_skin_bindings(*a));
}

TEST(GltfSkinBinding, WeightedJointPastTheSkinIsRejected) {
    std::optional<fastgltf::Asset> a;
    ASSERT_NO_FATAL_FAILURE(parse(skinned_triangle(7, 0.5F), a));
    EXPECT_EQ(g::max_weighted_joint(*a, a->meshes[0].primitives[0]), 7U);
    ASSERT_ERR(g::validate_skin_bindings(*a));
}

TEST(GltfSkinBinding, ZeroWeightJointPastTheSkinIsTolerated) {
    // Exporters pad unused influences; the shader clamps their indices.
    std::optional<fastgltf::Asset> a;
    ASSERT_NO_FATAL_FAILURE(parse(skinned_triangle(200, 0.0F), a));
    EXPECT_EQ(g::max_weighted_joint(*a, a->meshes[0].primitives[0]), 0U);
    EXPECT_OK(g::validate_skin_bindings(*a));
}

TEST(GltfSkinBinding, UnskinnedNodesAreNotChecked) {
    std::optional<fastgltf::Asset> a;
    ASSERT_NO_FATAL_FAILURE(parse(skinned_triangle(7, 0.5F, /*skinned_node=*/false), a));
    EXPECT_OK(g::validate_skin_bindings(*a));
}

} // namespace
