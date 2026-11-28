// Offline nature-asset generator (tools/asset-gen, R14 showcase): determinism,
// the generated .glb files pass the engine's glTF loader, sensible sizes, and
// the committed assets/meshes/nature/ files are the generator's output.

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "asset-gen/glb_writer.hpp"
#include "asset-gen/nature_models.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "support/expect_ok.hpp"

namespace {

namespace ag = quat::asset_gen;
using engine::scene::GltfLoader;

std::vector<std::byte> generate(std::size_t index, std::uint32_t seed) {
    return ag::write_glb(ag::nature_catalogue()[index].build(ag::asset_seed(seed, index)));
}

std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> out(chars.size());
    for (std::size_t i = 0; i < chars.size(); ++i) out[i] = static_cast<std::byte>(chars[i]);
    return out;
}

struct SizeLimits {
    float min_height;
    float max_height;
    float max_sink; // how far below the origin the model may reach
};

SizeLimits limits(ag::NatureKind kind) {
    switch (kind) {
    case ag::NatureKind::conifer: return {8.0F, 16.0F, 0.6F};
    case ag::NatureKind::broadleaf: return {8.0F, 15.0F, 0.6F};
    case ag::NatureKind::rock: return {0.5F, 4.0F, 1.0F};
    case ag::NatureKind::bush: return {0.6F, 2.0F, 0.2F};
    }
    return {};
}

TEST(AssetGen, SameSeedGivesTheSameBytes) {
    const auto catalogue = ag::nature_catalogue();
    ASSERT_GE(catalogue.size(), 12U);
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        EXPECT_EQ(generate(i, 1), generate(i, 1)) << catalogue[i].file;
        EXPECT_NE(generate(i, 1), generate(i, 2)) << catalogue[i].file << ": the seed is ignored";
    }
}

TEST(AssetGen, ModelsPassTheEngineLoader) {
    const auto catalogue = ag::nature_catalogue();
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        SCOPED_TRACE(std::string(catalogue[i].file));
        const ag::Model model = catalogue[i].build(ag::asset_seed(1, i));
        const std::vector<std::byte> bytes = ag::write_glb(model);
        EXPECT_LT(bytes.size(), 200U * 1024U);

        const auto mesh = GltfLoader::load_mesh_data_from_memory(bytes, ".");
        ASSERT_OK(mesh);
        EXPECT_EQ(mesh->indices.size(), model.triangle_count() * 3);
        EXPECT_GE(model.triangle_count(), 30U);
        EXPECT_LE(model.triangle_count(), 2000U);
        EXPECT_TRUE(mesh->skin.empty());
        for (const auto& v : mesh->vertices) {
            ASSERT_NEAR(glm::length(v.normal), 1.0F, 1e-3F);
        }

        // Origin at the base, metre scale.
        const SizeLimits lim = limits(catalogue[i].kind);
        EXPECT_LE(mesh->bounds.min.y, 0.05F);
        EXPECT_GE(mesh->bounds.min.y, -lim.max_sink);
        EXPECT_GE(mesh->bounds.max.y, lim.min_height);
        EXPECT_LE(mesh->bounds.max.y, lim.max_height);
        EXPECT_LT(std::abs(0.5F * (mesh->bounds.min.x + mesh->bounds.max.x)), 1.5F);
        EXPECT_LT(std::abs(0.5F * (mesh->bounds.min.z + mesh->bounds.max.z)), 1.5F);

        const auto materials = GltfLoader::load_material_data_from_memory(bytes, ".");
        ASSERT_OK(materials);
        ASSERT_EQ(materials->size(), model.materials.size());
        for (std::size_t m = 0; m < materials->size(); ++m) {
            EXPECT_EQ((*materials)[m].params.base_color_factor, model.materials[m].base_color);
            EXPECT_EQ((*materials)[m].params.roughness_factor, model.materials[m].roughness);
        }
    }
}

// Generated outward-facing triangles: a closed blob's normals point away from
// its centre (a winding error would light the inside).
TEST(AssetGen, BlobNormalsFaceOutward) {
    ag::Primitive prim;
    ag::BlobParams b;
    b.centre = glm::vec3(1.0F, 2.0F, 3.0F);
    b.radii = glm::vec3(1.5F, 1.0F, 1.2F);
    b.subdivisions = 2;
    ag::add_blob(prim, b);
    ASSERT_EQ(prim.triangle_count(), 320U);
    for (std::size_t t = 0; t < prim.triangle_count(); ++t) {
        const glm::vec3 c =
            (prim.positions[3 * t] + prim.positions[3 * t + 1] + prim.positions[3 * t + 2]) / 3.0F;
        EXPECT_GT(glm::dot(prim.normals[3 * t], c - b.centre), 0.0F) << "triangle " << t;
    }
}

// The committed files match what the generator makes today (compared as
// geometry, so the check tolerates last-bit libm differences between
// platforms). Regenerate with: quat-asset-gen assets/meshes/nature
TEST(AssetGen, CommittedAssetsAreCurrent) {
    const std::filesystem::path dir = std::filesystem::path(QUAT_PROJECT_ROOT) / "assets/meshes/nature";
    const auto catalogue = ag::nature_catalogue();
    for (std::size_t i = 0; i < catalogue.size(); ++i) {
        SCOPED_TRACE(std::string(catalogue[i].file));
        const std::vector<std::byte> committed = read_file(dir / catalogue[i].file);
        ASSERT_FALSE(committed.empty()) << "missing; run quat-asset-gen assets/meshes/nature";
        const auto want = GltfLoader::load_mesh_data_from_memory(generate(i, 1), ".");
        const auto have = GltfLoader::load_mesh_data_from_memory(committed, ".");
        ASSERT_OK(want);
        ASSERT_OK(have);
        ASSERT_EQ(have->vertices.size(), want->vertices.size());
        ASSERT_EQ(have->indices, want->indices);
        for (std::size_t v = 0; v < want->vertices.size(); ++v) {
            ASSERT_LT(glm::distance(have->vertices[v].position, want->vertices[v].position), 1e-3F)
                << "vertex " << v;
        }
    }
}

} // namespace
