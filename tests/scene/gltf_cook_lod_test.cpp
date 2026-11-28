// Cook-time level of detail and material rewrites (quat-cook --simplify,
// --blend-to-mask): a connected surface is reduced by the topology-preserving
// simplifier and keeps its shape; foliage made of disconnected cards is
// thinned to whole cards that grow to keep its coverage; BLEND materials
// become MASK.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/cook/gltf_cooker.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using engine::scene::GltfLoader;

struct Geometry {
    std::vector<float>         positions; // xyz
    std::vector<float>         normals;   // xyz
    std::vector<float>         uvs;       // uv
    std::vector<std::uint32_t> indices;
};

// A smooth 64 x 64 vertex height field, 16 m across (one connected surface).
Geometry hill() {
    constexpr int n = 64;
    Geometry g;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float x = static_cast<float>(i) / (n - 1) * 16.0F - 8.0F;
            const float z = static_cast<float>(j) / (n - 1) * 16.0F - 8.0F;
            const float y = 2.0F * std::exp(-(x * x + z * z) / 20.0F);
            const float dydx = -y * x / 10.0F;
            const float dydz = -y * z / 10.0F;
            const glm::vec3 nrm = glm::normalize(glm::vec3(-dydx, 1.0F, -dydz));
            g.positions.insert(g.positions.end(), {x, y, z});
            g.normals.insert(g.normals.end(), {nrm.x, nrm.y, nrm.z});
            g.uvs.insert(g.uvs.end(), {static_cast<float>(i) / (n - 1), static_cast<float>(j) / (n - 1)});
        }
    }
    for (int j = 0; j + 1 < n; ++j) {
        for (int i = 0; i + 1 < n; ++i) {
            const auto v = static_cast<std::uint32_t>(j * n + i);
            g.indices.insert(g.indices.end(), {v, v + n, v + 1, v + 1, v + n, v + n + 1});
        }
    }
    return g;
}

// 4096 separate 6 cm leaf cards (two triangles each) filling a 4 m cube.
Geometry leaves() {
    Geometry g;
    std::uint32_t state = 12345U;
    const auto rnd = [&] {
        state = state * 1664525U + 1013904223U;
        return static_cast<float>(state >> 8) / static_cast<float>(1U << 24);
    };
    for (int k = 0; k < 4096; ++k) {
        const glm::vec3 c(rnd() * 4.0F - 2.0F, rnd() * 4.0F, rnd() * 4.0F - 2.0F);
        const auto base = static_cast<std::uint32_t>(g.positions.size() / 3);
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& q : corners) {
            g.positions.insert(g.positions.end(), {c.x + q[0] * 0.03F, c.y + q[1] * 0.03F, c.z});
            g.normals.insert(g.normals.end(), {0.0F, 0.0F, 1.0F});
            g.uvs.insert(g.uvs.end(), {q[0] * 0.5F + 0.5F, q[1] * 0.5F + 0.5F});
        }
        g.indices.insert(g.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return g;
}

void append(std::vector<std::uint8_t>& bin, const void* data, std::size_t bytes) {
    const std::size_t at = bin.size();
    bin.resize(at + bytes);
    std::memcpy(bin.data() + at, data, bytes);
}

// One primitive with a material of `alpha_mode`, as .gltf + .bin.
fs::path write_gltf(const fs::path& dir, const Geometry& g, const char* alpha_mode) {
    std::vector<std::uint8_t> bin;
    append(bin, g.positions.data(), g.positions.size() * 4);
    append(bin, g.normals.data(), g.normals.size() * 4);
    append(bin, g.uvs.data(), g.uvs.size() * 4);
    append(bin, g.indices.data(), g.indices.size() * 4);
    {
        std::ofstream f(dir / "model.bin", std::ios::binary);
        f.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    }
    glm::vec3 lo(1e9F), hi(-1e9F);
    for (std::size_t i = 0; i < g.positions.size(); i += 3) {
        const glm::vec3 p(g.positions[i], g.positions[i + 1], g.positions[i + 2]);
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const std::size_t vc = g.positions.size() / 3;
    const std::size_t p_len = g.positions.size() * 4;
    const std::size_t n_len = g.normals.size() * 4;
    const std::size_t u_len = g.uvs.size() * 4;
    const std::size_t i_len = g.indices.size() * 4;
    std::ostringstream j;
    j << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},)"
      << R"("indices":3,"material":0}]}],)"
      << R"("materials":[{"alphaMode":")" << alpha_mode << R"("}],)"
      << R"("buffers":[{"uri":"model.bin","byteLength":)" << bin.size() << "}],"
      << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" << p_len << "},"
      << R"({"buffer":0,"byteOffset":)" << p_len << R"(,"byteLength":)" << n_len << "},"
      << R"({"buffer":0,"byteOffset":)" << p_len + n_len << R"(,"byteLength":)" << u_len << "},"
      << R"({"buffer":0,"byteOffset":)" << p_len + n_len + u_len << R"(,"byteLength":)" << i_len << "}],"
      << R"("accessors":[)"
      << R"({"bufferView":0,"componentType":5126,"count":)" << vc << R"(,"type":"VEC3","min":[)"
      << lo.x << "," << lo.y << "," << lo.z << "],\"max\":[" << hi.x << "," << hi.y << "," << hi.z
      << "]},"
      << R"({"bufferView":1,"componentType":5126,"count":)" << vc << R"(,"type":"VEC3"},)"
      << R"({"bufferView":2,"componentType":5126,"count":)" << vc << R"(,"type":"VEC2"},)"
      << R"({"bufferView":3,"componentType":5125,"count":)" << g.indices.size()
      << R"(,"type":"SCALAR"}]})";
    const fs::path path = dir / "model.gltf";
    std::ofstream(path) << j.str();
    return path;
}

// The JSON chunk of a binary glTF.
nlohmann::json glb_json(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 20) return {};
    std::uint32_t length = 0;
    std::memcpy(&length, bytes.data() + 12, 4);
    if (20 + static_cast<std::size_t>(length) > bytes.size()) return {};
    return nlohmann::json::parse(bytes.begin() + 20, bytes.begin() + 20 + length, nullptr, false);
}

double total_area(const engine::scene::MeshData& m) {
    double area = 0.0;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const glm::vec3 a = m.vertices[m.indices[t]].position;
        const glm::vec3 b = m.vertices[m.indices[t + 1]].position;
        const glm::vec3 c = m.vertices[m.indices[t + 2]].position;
        area += 0.5 * static_cast<double>(glm::length(glm::cross(b - a, c - a)));
    }
    return area;
}

engine::cook::CookOptions options(float ratio) {
    engine::cook::CookOptions o;
    o.textures.enabled = false;
    o.meshes.simplify_ratio = ratio;
    return o;
}

} // namespace

TEST(GltfCookLod, ConnectedSurfaceIsSimplifiedAndKeepsItsShape) {
    quat::test::TempDir dir("gltf-cook-lod-hill");
    const Geometry g = hill();
    const fs::path src = write_gltf(dir.path(), g, "OPAQUE");
    const fs::path out = dir.path() / "hill_lod.glb";
    auto stats = engine::cook::cook_gltf(src, out, options(0.1F));
    ASSERT_OK(stats);
    EXPECT_EQ(stats->triangles_before, g.indices.size() / 3);

    auto before = GltfLoader::load_mesh_data(src);
    auto after = GltfLoader::load_mesh_data(out);
    ASSERT_OK(before);
    ASSERT_OK(after);
    const std::size_t tris_before = before->indices.size() / 3;
    const std::size_t tris_after = after->indices.size() / 3;
    EXPECT_GT(tris_after, 0U);
    EXPECT_LE(tris_after, tris_before / 10 + 2);
    EXPECT_EQ(stats->triangles_after, tris_after);
    EXPECT_LT(after->vertices.size(), before->vertices.size() / 4); // unused vertices dropped
    // Same extent (the border and the peak survive within the error bound).
    EXPECT_LT(glm::length(after->bounds.min - before->bounds.min), 0.05F);
    EXPECT_LT(glm::length(after->bounds.max - before->bounds.max), 0.05F);
    // Every kept vertex is still on the surface.
    for (const engine::asset::Vertex& v : after->vertices) {
        const glm::vec3 p = v.position;
        EXPECT_NEAR(p.y, 2.0F * std::exp(-(p.x * p.x + p.z * p.z) / 20.0F), 1e-3F);
    }
}

TEST(GltfCookLod, DisconnectedFoliageIsThinnedAndKeepsItsCoverage) {
    quat::test::TempDir dir("gltf-cook-lod-leaves");
    const Geometry g = leaves();
    const fs::path src = write_gltf(dir.path(), g, "MASK");
    const fs::path out = dir.path() / "leaves_lod.glb";
    ASSERT_OK(engine::cook::cook_gltf(src, out, options(0.1F)));
    auto before = GltfLoader::load_mesh_data(src);
    auto after = GltfLoader::load_mesh_data(out);
    ASSERT_OK(before);
    ASSERT_OK(after);
    // Topology-preserving simplification cannot go below two triangles per
    // card; thinning keeps whole cards, about 10 % of them.
    const std::size_t tris_before = before->indices.size() / 3;
    const std::size_t tris_after = after->indices.size() / 3;
    EXPECT_EQ(tris_after % 2, 0U); // whole cards only
    EXPECT_GT(tris_after, tris_before / 20);
    EXPECT_LT(tris_after, tris_before / 6);
    // The kept cards grow so the total leaf area (coverage from afar) stays.
    const double area_before = total_area(*before);
    const double area_after = total_area(*after);
    EXPECT_NEAR(area_after / area_before, 1.0, 0.2);
    // Cards stay inside the canopy's volume (grown about their own centre).
    EXPECT_LT(glm::length(after->bounds.max - before->bounds.max), 0.25F);
    EXPECT_LT(glm::length(after->bounds.min - before->bounds.min), 0.25F);
}

TEST(GltfCookLod, RatioOneLeavesTheTrianglesAlone) {
    quat::test::TempDir dir("gltf-cook-lod-off");
    const Geometry g = hill();
    const fs::path src = write_gltf(dir.path(), g, "OPAQUE");
    const fs::path out = dir.path() / "hill.glb";
    ASSERT_OK(engine::cook::cook_gltf(src, out, options(1.0F)));
    auto after = GltfLoader::load_mesh_data(out);
    ASSERT_OK(after);
    EXPECT_EQ(after->indices.size(), g.indices.size());
}

TEST(GltfCookLod, BlendMaterialsBecomeMaskOnlyWhenAsked) {
    quat::test::TempDir dir("gltf-cook-blend-mask");
    const fs::path src = write_gltf(dir.path(), leaves(), "BLEND");

    const fs::path kept = dir.path() / "kept.glb";
    ASSERT_OK(engine::cook::cook_gltf(src, kept, options(1.0F)));
    nlohmann::json kept_json = glb_json(kept);
    ASSERT_EQ(kept_json["materials"].size(), 1U);
    EXPECT_EQ(kept_json["materials"][0]["alphaMode"], "BLEND");

    engine::cook::CookOptions o = options(1.0F);
    o.materials.blend_to_mask_cutoff = 0.3F;
    const fs::path masked = dir.path() / "masked.glb";
    ASSERT_OK(engine::cook::cook_gltf(src, masked, o));
    nlohmann::json json = glb_json(masked);
    ASSERT_EQ(json["materials"].size(), 1U);
    EXPECT_EQ(json["materials"][0]["alphaMode"], "MASK");
    ASSERT_TRUE(json["materials"][0]["alphaCutoff"].is_number());
    EXPECT_NEAR(json["materials"][0]["alphaCutoff"].get<double>(), 0.3, 1e-6);
}
