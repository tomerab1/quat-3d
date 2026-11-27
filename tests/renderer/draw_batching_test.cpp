// Draw batching for instanced rendering (R9.1): grouping, ordering, skinned
// pass-through, and the per-instance normal matrix (audit A13).

#include <array>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/draw_batching.hpp"

namespace {

using engine::renderer::batch_draws;
using engine::renderer::DrawItem;
using engine::renderer::InstanceData;
using engine::renderer::InstancedDrawList;
using engine::renderer::make_instance_data;
using engine::renderer::unbatched_draws;

glm::mat4 at(float x) { return glm::translate(glm::mat4(1.0F), glm::vec3(x, 0.0F, 0.0F)); }

float x_of(const InstanceData& i) { return i.model[3][0]; }

// Every draw's range is inside the instance array and ranges tile it exactly.
void expect_well_formed(const InstancedDrawList& list) {
    std::uint32_t next = 0;
    for (const auto& d : list.draws) {
        EXPECT_EQ(d.first_instance, next);
        EXPECT_GT(d.instance_count, 0U);
        next += d.instance_count;
    }
    EXPECT_EQ(next, list.instances.size());
}

// The world normal the shaders compute: normal matrix columns times n.
glm::vec3 shader_normal(const InstanceData& i, const glm::vec3& n) {
    return glm::vec3(i.normal[0]) * n.x + glm::vec3(i.normal[1]) * n.y +
           glm::vec3(i.normal[2]) * n.z;
}

} // namespace

TEST(DrawBatching, MergesSameMeshAndMaterialInFirstAppearanceOrder) {
    engine::asset::MeshAsset tree;
    engine::asset::MeshAsset rock;
    engine::asset::MaterialAsset bark;
    engine::asset::MaterialAsset stone;

    const std::array<DrawItem, 6> items{{
        {&rock, &stone, at(0.0F)},
        {&tree, &bark, at(1.0F)},
        {&rock, &stone, at(2.0F)},
        {&tree, &stone, at(3.0F)}, // same mesh, other material: its own batch
        {&tree, &bark, at(4.0F)},
        {&rock, &stone, at(5.0F)},
    }};
    const InstancedDrawList list = batch_draws(items);
    expect_well_formed(list);

    ASSERT_EQ(list.draws.size(), 3U);
    EXPECT_EQ(list.draws[0].mesh, &rock);
    EXPECT_EQ(list.draws[0].material, &stone);
    EXPECT_EQ(list.draws[0].instance_count, 3U);
    EXPECT_EQ(list.draws[1].mesh, &tree);
    EXPECT_EQ(list.draws[1].material, &bark);
    EXPECT_EQ(list.draws[1].instance_count, 2U);
    EXPECT_EQ(list.draws[2].mesh, &tree);
    EXPECT_EQ(list.draws[2].material, &stone);
    EXPECT_EQ(list.draws[2].instance_count, 1U);

    // Instances are grouped per draw and keep the items' relative order.
    const std::vector<float> xs{0.0F, 2.0F, 5.0F, 1.0F, 4.0F, 3.0F};
    ASSERT_EQ(list.instances.size(), xs.size());
    for (std::size_t i = 0; i < xs.size(); ++i) EXPECT_EQ(x_of(list.instances[i]), xs[i]);
}

TEST(DrawBatching, NullMaterialIsItsOwnKeyAndNullMeshIsDropped) {
    engine::asset::MeshAsset box;
    engine::asset::MaterialAsset paint;
    const std::array<DrawItem, 4> items{{
        {&box, nullptr, at(0.0F)},
        {nullptr, &paint, at(1.0F)},
        {&box, &paint, at(2.0F)},
        {&box, nullptr, at(3.0F)},
    }};
    const InstancedDrawList list = batch_draws(items);
    expect_well_formed(list);
    ASSERT_EQ(list.draws.size(), 2U);
    EXPECT_EQ(list.draws[0].material, nullptr);
    EXPECT_EQ(list.draws[0].instance_count, 2U);
    EXPECT_EQ(list.draws[1].material, &paint);
    EXPECT_EQ(list.instances.size(), 3U);
}

TEST(DrawBatching, SkinnedDrawsStaySingleAndKeepTheirBuffer) {
    engine::asset::MeshAsset hero;
    engine::asset::MaterialAsset skin;
    // Stand-in handles: batching only compares and copies them.
    const auto buf_a = reinterpret_cast<VkBuffer>(std::uintptr_t{0x1000});
    const auto buf_b = reinterpret_cast<VkBuffer>(std::uintptr_t{0x2000});

    std::array<DrawItem, 4> items{{
        {&hero, &skin, glm::mat4(1.0F), buf_a},
        {&hero, &skin, at(1.0F)},
        {&hero, &skin, glm::mat4(1.0F), buf_b},
        {&hero, &skin, at(2.0F)},
    }};
    const InstancedDrawList list = batch_draws(items);
    expect_well_formed(list);
    ASSERT_EQ(list.draws.size(), 3U);
    EXPECT_EQ(list.draws[0].skinned_vertices, buf_a);
    EXPECT_EQ(list.draws[0].instance_count, 1U);
    EXPECT_EQ(list.draws[1].skinned_vertices, VK_NULL_HANDLE);
    EXPECT_EQ(list.draws[1].instance_count, 2U); // the two static draws merged
    EXPECT_EQ(list.draws[2].skinned_vertices, buf_b);
    EXPECT_EQ(list.draws[2].instance_count, 1U);
}

TEST(DrawBatching, IsDeterministic) {
    std::array<engine::asset::MeshAsset, 3> meshes;
    std::vector<DrawItem> items;
    for (int i = 0; i < 64; ++i) {
        items.push_back({&meshes[static_cast<std::size_t>((i * 7) % 3)], nullptr,
                         at(static_cast<float>(i))});
    }
    const InstancedDrawList a = batch_draws(items);
    const InstancedDrawList b = batch_draws(items);
    ASSERT_EQ(a.draws.size(), b.draws.size());
    for (std::size_t i = 0; i < a.draws.size(); ++i) {
        EXPECT_EQ(a.draws[i].mesh, b.draws[i].mesh);
        EXPECT_EQ(a.draws[i].first_instance, b.draws[i].first_instance);
        EXPECT_EQ(a.draws[i].instance_count, b.draws[i].instance_count);
    }
    ASSERT_EQ(a.instances.size(), b.instances.size());
    for (std::size_t i = 0; i < a.instances.size(); ++i) {
        EXPECT_EQ(x_of(a.instances[i]), x_of(b.instances[i]));
    }
}

TEST(DrawBatching, UnbatchedKeepsOrderOneInstanceEach) {
    engine::asset::MeshAsset pane;
    engine::asset::MaterialAsset glass;
    const std::array<DrawItem, 4> items{{
        {&pane, &glass, at(3.0F)},
        {&pane, &glass, at(1.0F)},
        {nullptr, &glass, at(9.0F)},
        {&pane, &glass, at(2.0F)},
    }};
    const InstancedDrawList list = unbatched_draws(items);
    expect_well_formed(list);
    ASSERT_EQ(list.draws.size(), 3U);
    EXPECT_EQ(x_of(list.instances[0]), 3.0F);
    EXPECT_EQ(x_of(list.instances[1]), 1.0F);
    EXPECT_EQ(x_of(list.instances[2]), 2.0F);
}

TEST(DrawBatching, EmptyInputGivesEmptyList) {
    const InstancedDrawList list = batch_draws({});
    EXPECT_TRUE(list.draws.empty());
    EXPECT_TRUE(list.instances.empty());
}

// Audit A13: under non-uniform scale the normal must follow the inverse
// transpose, not the model 3x3.
TEST(InstanceData, NormalMatrixHandlesNonUniformScale) {
    // Plane y + z = 0 scaled by (1, 1, 3) becomes 3y + z = 0.
    const glm::mat4 m = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 1.0F, 3.0F));
    const InstanceData inst = make_instance_data(m);
    const glm::vec3 n = glm::normalize(shader_normal(inst, glm::normalize(glm::vec3(0, 1, 1))));
    const glm::vec3 want = glm::normalize(glm::vec3(0.0F, 3.0F, 1.0F));
    EXPECT_NEAR(glm::dot(n, want), 1.0F, 1e-5F);
    EXPECT_EQ(inst.normal[0].w, 1.0F);
    EXPECT_EQ(inst.model, m);
}

TEST(InstanceData, NormalMatrixMatchesInverseTransposeForGeneralTransforms) {
    const glm::mat4 m = glm::translate(glm::mat4(1.0F), glm::vec3(4.0F, -2.0F, 7.0F)) *
                        glm::rotate(glm::mat4(1.0F), 0.7F, glm::normalize(glm::vec3(1, 2, 3))) *
                        glm::scale(glm::mat4(1.0F), glm::vec3(0.5F, 2.0F, -1.5F));
    const InstanceData inst = make_instance_data(m);
    const glm::mat3 inv_t = glm::transpose(glm::inverse(glm::mat3(m)));
    for (const glm::vec3 n : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1),
                              glm::normalize(glm::vec3(1, -2, 0.5F))}) {
        const glm::vec3 got = glm::normalize(shader_normal(inst, n));
        const glm::vec3 want = glm::normalize(inv_t * n);
        // Same direction (not flipped) even though this transform mirrors.
        EXPECT_NEAR(glm::dot(got, want), 1.0F, 1e-5F);
    }
    EXPECT_EQ(inst.normal[0].w, -1.0F); // det < 0
    EXPECT_EQ(inst.normal[1].w, 0.0F);
    EXPECT_EQ(inst.normal[2].w, 0.0F);
}

// Mirroring: the shaders rebuild the bitangent as cross(N, T) * w with the
// tangent's w multiplied by normal[0].w; that must equal the transformed
// object-space bitangent so normal maps keep their handedness.
TEST(InstanceData, MirroredTransformKeepsTangentHandedness) {
    const glm::vec3 n(0.0F, 0.0F, 1.0F);
    const glm::vec3 t(1.0F, 0.0F, 0.0F);
    const float w = 1.0F;
    const glm::vec3 b = glm::cross(n, t) * w;

    for (const glm::vec3 s : {glm::vec3(-1, 1, 1), glm::vec3(1, -2, 1), glm::vec3(1, 1, -3),
                              glm::vec3(2, 0.5F, 1)}) {
        const glm::mat4 m = glm::rotate(glm::mat4(1.0F), 0.4F, glm::vec3(0, 1, 0)) *
                            glm::scale(glm::mat4(1.0F), s);
        const InstanceData inst = make_instance_data(m);
        const glm::vec3 wn = glm::normalize(shader_normal(inst, n));
        const glm::vec3 wt = glm::normalize(glm::mat3(m) * t);
        const float ww = w * inst.normal[0].w;
        const glm::vec3 wb = glm::cross(wn, wt) * ww;
        const glm::vec3 want = glm::normalize(glm::mat3(m) * b);
        EXPECT_GT(glm::dot(wb, want), 0.99F) << "scale " << s.x << "," << s.y << "," << s.z;
    }
}

TEST(InstanceData, SingularTransformStaysFinite) {
    // Flattened onto the XZ plane: the cofactor still yields a usable normal.
    const glm::mat4 m = glm::scale(glm::mat4(1.0F), glm::vec3(2.0F, 0.0F, 2.0F));
    const InstanceData inst = make_instance_data(m);
    const glm::vec3 up = shader_normal(inst, glm::vec3(0.0F, 1.0F, 0.0F));
    EXPECT_GT(up.y, 0.0F);
    EXPECT_EQ(inst.normal[0].w, 1.0F);
}
