// The asset cooker (R17) round trip: a glTF with PNG textures and a float mesh
// is cooked to KHR_texture_basisu + EXT_meshopt_compression +
// KHR_mesh_quantization, and the engine's loader reads back the same
// triangles (within quantisation error) and the same images (within codec
// error). Corrupt compressed geometry is rejected, not decoded.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <gtest/gtest.h>
#include <stb_image_write.h>

#include "engine/cook/gltf_cooker.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "support/expect_ok.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using engine::scene::GltfLoader;
using engine::scene::MeshData;

constexpr int k_grid = 24;      // vertices per side
constexpr float k_step = 0.25F; // grid spacing (x / z)
constexpr int k_tex = 32;

glm::vec3 grid_position(int i, int j) {
    const float x = static_cast<float>(i) * k_step;
    const float z = static_cast<float>(j) * k_step;
    return {x, 0.5F * std::sin(x) * std::cos(z), z};
}

void write_file(const fs::path& p, const void* data, std::size_t size) {
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}

void write_png(const fs::path& p, const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> png;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* out = static_cast<std::vector<std::uint8_t>*>(ctx);
            out->insert(out->end(), static_cast<std::uint8_t*>(data),
                        static_cast<std::uint8_t*>(data) + size);
        },
        &png, k_tex, k_tex, 4, rgba.data(), k_tex * 4);
    write_file(p, png.data(), png.size());
}

std::vector<std::uint8_t> albedo_pixels() {
    std::vector<std::uint8_t> px(k_tex * k_tex * 4);
    for (int y = 0; y < k_tex; ++y) {
        for (int x = 0; x < k_tex; ++x) {
            std::uint8_t* p = &px[static_cast<std::size_t>(y * k_tex + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 8);
            p[1] = static_cast<std::uint8_t>(y * 8);
            p[2] = 128;
            p[3] = 255;
        }
    }
    return px;
}

std::vector<std::uint8_t> normal_pixels() {
    std::vector<std::uint8_t> px(k_tex * k_tex * 4);
    for (int i = 0; i < k_tex * k_tex; ++i) {
        const float nx = 0.3F * std::sin(static_cast<float>(i % k_tex) * 0.3F);
        const float ny = 0.3F * std::cos(static_cast<float>(i / k_tex) * 0.3F);
        const float n[3] = {nx, ny, std::sqrt(1.0F - nx * nx - ny * ny)};
        for (int c = 0; c < 3; ++c) {
            px[static_cast<std::size_t>(i) * 4 + c] =
                static_cast<std::uint8_t>(std::lround((n[c] * 0.5F + 0.5F) * 255.0F));
        }
        px[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return px;
}

// A k_grid x k_grid height-field mesh (positions, normals, tangents, UVs,
// uint16 indices) with a base colour + normal map material, as .gltf + .bin
// + two PNGs.
fs::path write_source_gltf(const fs::path& dir) {
    std::vector<float> pos, nrm, tan, uv;
    for (int j = 0; j < k_grid; ++j) {
        for (int i = 0; i < k_grid; ++i) {
            const glm::vec3 p = grid_position(i, j);
            const glm::vec3 dx = grid_position(i + 1, j) - grid_position(i - 1, j);
            const glm::vec3 dz = grid_position(i, j + 1) - grid_position(i, j - 1);
            const glm::vec3 n = glm::normalize(glm::cross(dz, dx));
            const glm::vec3 t = glm::normalize(dx);
            pos.insert(pos.end(), {p.x, p.y, p.z});
            nrm.insert(nrm.end(), {n.x, n.y, n.z});
            tan.insert(tan.end(), {t.x, t.y, t.z, 1.0F});
            uv.insert(uv.end(), {static_cast<float>(i) / (k_grid - 1),
                                 static_cast<float>(j) / (k_grid - 1)});
        }
    }
    std::vector<std::uint16_t> idx;
    for (int j = 0; j + 1 < k_grid; ++j) {
        for (int i = 0; i + 1 < k_grid; ++i) {
            const auto v = static_cast<std::uint16_t>(j * k_grid + i);
            idx.insert(idx.end(), {v, static_cast<std::uint16_t>(v + k_grid),
                                   static_cast<std::uint16_t>(v + 1),
                                   static_cast<std::uint16_t>(v + 1),
                                   static_cast<std::uint16_t>(v + k_grid),
                                   static_cast<std::uint16_t>(v + k_grid + 1)});
        }
    }
    std::vector<std::uint8_t> bin;
    const auto append = [&](const auto& v) {
        const std::size_t at = bin.size();
        const std::size_t bytes = v.size() * sizeof(v[0]);
        bin.resize(at + (bytes + 3) / 4 * 4);
        std::memcpy(bin.data() + at, v.data(), bytes);
        return std::pair{at, bytes};
    };
    const auto [p_off, p_len] = append(pos);
    const auto [n_off, n_len] = append(nrm);
    const auto [t_off, t_len] = append(tan);
    const auto [u_off, u_len] = append(uv);
    const auto [i_off, i_len] = append(idx);
    write_file(dir / "grid.bin", bin.data(), bin.size());
    write_png(dir / "albedo.png", albedo_pixels());
    write_png(dir / "normal.png", normal_pixels());

    const int vc = k_grid * k_grid;
    float ymin = 1e9F, ymax = -1e9F;
    for (std::size_t k = 1; k < pos.size(); k += 3) {
        ymin = std::min(ymin, pos[k]);
        ymax = std::max(ymax, pos[k]);
    }
    std::ostringstream j;
    j << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3},"indices":4,"material":0}]}],)"
      << R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"normalTexture":{"index":1}}],)"
      << R"("textures":[{"source":0},{"source":1}],"images":[{"uri":"albedo.png"},{"uri":"normal.png"}],)"
      << R"("buffers":[{"uri":"grid.bin","byteLength":)" << bin.size() << "}],"
      << R"("bufferViews":[)"
      << R"({"buffer":0,"byteOffset":)" << p_off << R"(,"byteLength":)" << p_len << "},"
      << R"({"buffer":0,"byteOffset":)" << n_off << R"(,"byteLength":)" << n_len << "},"
      << R"({"buffer":0,"byteOffset":)" << t_off << R"(,"byteLength":)" << t_len << "},"
      << R"({"buffer":0,"byteOffset":)" << u_off << R"(,"byteLength":)" << u_len << "},"
      << R"({"buffer":0,"byteOffset":)" << i_off << R"(,"byteLength":)" << i_len << "}],"
      << R"("accessors":[)"
      << R"({"bufferView":0,"componentType":5126,"count":)" << vc << R"(,"type":"VEC3","min":[0,)"
      << ymin << ",0],\"max\":[" << (k_grid - 1) * k_step << "," << ymax << ","
      << (k_grid - 1) * k_step << "]},"
      << R"({"bufferView":1,"componentType":5126,"count":)" << vc << R"(,"type":"VEC3"},)"
      << R"({"bufferView":2,"componentType":5126,"count":)" << vc << R"(,"type":"VEC4"},)"
      << R"({"bufferView":3,"componentType":5126,"count":)" << vc << R"(,"type":"VEC2"},)"
      << R"({"bufferView":4,"componentType":5123,"count":)" << idx.size() << R"(,"type":"SCALAR"}]})";
    const std::string json = j.str();
    write_file(dir / "grid.gltf", json.data(), json.size());
    return dir / "grid.gltf";
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Where the POSITION stream's bufferView starts in a cooked .gltf, and its
// EXT_meshopt_compression object.
struct PositionView {
    std::size_t begin = std::string::npos;
    std::size_t ext = std::string::npos;
};

// Replaces the number of the first `"field":` at or after `from`.
void set_number(std::string& json, std::size_t from, const std::string& field,
                const std::string& value) {
    const std::string key = "\"" + field + "\":";
    const std::size_t at = json.find(key, from);
    ASSERT_NE(at, std::string::npos) << field;
    const std::size_t begin = at + key.size();
    json.replace(begin, json.find_first_not_of("0123456789", begin) - begin, value);
}

// A triangle as the grid (i, j) of its vertices, rotated so the smallest
// vertex comes first (winding preserved).
using Tri = std::array<int, 3>;

std::vector<Tri> grid_triangles(const MeshData& m) {
    std::vector<Tri> out;
    for (std::size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        Tri tri{};
        for (int k = 0; k < 3; ++k) {
            const glm::vec3 p = m.vertices[m.indices[t + k]].position;
            tri[k] = static_cast<int>(std::lround(p.z / k_step)) * k_grid +
                     static_cast<int>(std::lround(p.x / k_step));
        }
        std::rotate(tri.begin(), std::min_element(tri.begin(), tri.end()), tri.end());
        out.push_back(tri);
    }
    std::sort(out.begin(), out.end());
    return out;
}

double rmse(const std::vector<std::uint8_t>& a, std::span<const std::byte> b, int c0, int c1) {
    double sum = 0;
    for (std::size_t i = 0; i < a.size() / 4; ++i) {
        for (int c = c0; c <= c1; ++c) {
            const double d = a[i * 4 + c] - static_cast<double>(std::to_integer<std::uint8_t>(b[i * 4 + c]));
            sum += d * d;
        }
    }
    return std::sqrt(sum / static_cast<double>(a.size() / 4 * static_cast<std::size_t>(c1 - c0 + 1)));
}

} // namespace

TEST(GltfCook, CookedGlbLoadsTheSameGeometryAndTextures) {
    quat::test::TempDir dir("gltf-cook");
    const fs::path src = write_source_gltf(dir.path());
    const fs::path out = dir.path() / "grid_cooked.glb";
    engine::cook::CookOptions options;
    options.textures.threads = 1;
    auto stats = engine::cook::cook_gltf(src, out, options);
    ASSERT_OK(stats);
    EXPECT_EQ(stats->images_encoded, 2U);
    EXPECT_EQ(stats->primitives_cooked, 1U);
    EXPECT_LT(stats->mesh_bytes_after, stats->mesh_bytes_before / 2);

    const std::string glb = read_text(out);
    for (const char* ext : {"KHR_texture_basisu", "EXT_meshopt_compression",
                            "KHR_mesh_quantization", "quat_normal_xy"}) {
        EXPECT_NE(glb.find(ext), std::string::npos) << ext;
    }

    auto before = GltfLoader::load_mesh_data(src);
    auto after = GltfLoader::load_mesh_data(out);
    ASSERT_OK(before);
    ASSERT_OK(after);
    EXPECT_EQ(grid_triangles(*before), grid_triangles(*after));
    EXPECT_EQ(after->vertices.size(), before->vertices.size());
    for (const engine::asset::Vertex& v : after->vertices) {
        const int i = static_cast<int>(std::lround(v.position.x / k_step));
        const int j = static_cast<int>(std::lround(v.position.z / k_step));
        const engine::asset::Vertex& ref = before->vertices[static_cast<std::size_t>(j * k_grid + i)];
        EXPECT_LT(glm::length(v.position - ref.position), 1e-3F);
        EXPECT_LT(glm::length(v.normal - ref.normal), 0.03F);
        EXPECT_LT(glm::length(glm::vec3(v.tangent) - glm::vec3(ref.tangent)), 0.03F);
        EXPECT_EQ(v.tangent.w, ref.tangent.w);
        EXPECT_LT(glm::length(v.uv - ref.uv), 1e-4F);
    }

    // No device caps: the KTX2 images decode to RGBA8 with their mip chains.
    auto textures = GltfLoader::load_texture_data(out);
    auto materials = GltfLoader::load_material_data(out);
    ASSERT_OK(textures);
    ASSERT_OK(materials);
    ASSERT_EQ(materials->size(), 1U);
    const auto& albedo = (*textures)[(*materials)[0].base_color_image];
    const auto& normal = (*textures)[(*materials)[0].normal_image];
    ASSERT_EQ(albedo.levels.size(), 6U);
    EXPECT_EQ(albedo.color_space, engine::asset::TextureColorSpace::srgb);
    EXPECT_LT(rmse(albedo_pixels(), albedo.pixels.span(), 0, 2), 14.0);
    ASSERT_EQ(normal.swizzle, engine::asset::TextureSwizzle::ra01);
    std::vector<std::byte> xy(normal.pixels.begin(),
                              normal.pixels.begin() + static_cast<std::ptrdiff_t>(normal.levels[0].size));
    for (std::size_t i = 0; i < xy.size() / 4; ++i) xy[i * 4 + 1] = xy[i * 4 + 3];
    EXPECT_LT(rmse(normal_pixels(), xy, 0, 1), 4.0);
}

// Cooking a cooked file: the runtime's parse decodes the meshopt streams, the
// cook re-encodes them and keeps the KTX2 images.
TEST(GltfCook, CookedOutputCanBeCookedAgain) {
    quat::test::TempDir dir("gltf-cook-again");
    const fs::path src = write_source_gltf(dir.path());
    const fs::path once = dir.path() / "once.glb";
    const fs::path twice = dir.path() / "twice.glb";
    engine::cook::CookOptions options;
    options.textures.threads = 1;
    ASSERT_OK(engine::cook::cook_gltf(src, once, options));
    auto again = engine::cook::cook_gltf(once, twice, options);
    ASSERT_OK(again);
    EXPECT_EQ(again->images_encoded, 0U); // already KTX2
    auto before = GltfLoader::load_mesh_data(src);
    auto after = GltfLoader::load_mesh_data(twice);
    ASSERT_OK(before);
    ASSERT_OK(after);
    EXPECT_EQ(grid_triangles(*before), grid_triangles(*after));
    ASSERT_OK(GltfLoader::load_texture_data(twice));
}

TEST(GltfCook, UncompressedOutputStillLoads) {
    quat::test::TempDir dir("gltf-cook-raw");
    const fs::path src = write_source_gltf(dir.path());
    const fs::path out = dir.path() / "raw.gltf";
    engine::cook::CookOptions options;
    options.textures.enabled = false;
    options.meshes.compress = false;
    ASSERT_OK(engine::cook::cook_gltf(src, out, options));
    EXPECT_EQ(read_text(out).find("EXT_meshopt_compression"), std::string::npos);
    auto before = GltfLoader::load_mesh_data(src);
    auto after = GltfLoader::load_mesh_data(out);
    ASSERT_OK(before);
    ASSERT_OK(after);
    EXPECT_EQ(grid_triangles(*before), grid_triangles(*after));
}

TEST(GltfCook, CorruptCompressedGeometryIsRejected) {
    quat::test::TempDir dir("gltf-cook-bad");
    const fs::path src = write_source_gltf(dir.path());
    const fs::path out = dir.path() / "cooked.gltf";
    engine::cook::CookOptions options;
    options.textures.enabled = false;
    ASSERT_OK(engine::cook::cook_gltf(src, out, options));
    ASSERT_OK(GltfLoader::load_mesh_data(out));

    // Edits to the POSITION stream's bufferView (the only 12-byte stride).
    const std::string original = read_text(out);
    const auto view_of_positions = [](const std::string& json) {
        PositionView v;
        v.ext = json.rfind("\"EXT_meshopt_compression\":{",
                           json.find("\"byteStride\":12,\"count\""));
        v.begin = json.rfind("{\"buffer\":", v.ext);
        return v;
    };
    const auto load_edited = [&](auto edit) {
        std::string json = original;
        edit(json, view_of_positions(json));
        write_file(out, json.data(), json.size());
        return GltfLoader::load_mesh_data(out);
    };
    const auto expect_error = [](const auto& result, const char* text) {
        ASSERT_ERR(result);
        EXPECT_NE(result.error().message.find(text), std::string::npos) << result.error().message;
    };

    // A compressed range past the end of its buffer.
    EXPECT_ERR(load_edited([](std::string& json, const PositionView& v) {
        set_number(json, v.ext, "byteLength", "999999999");
    }));
    // Decompression bombs: far more decoded bytes than 1024x the compressed
    // ones, by shrinking the source or by raising the count (with the view's
    // byteLength kept consistent so only the expansion bound can object).
    expect_error(load_edited([](std::string& json, const PositionView& v) {
                     set_number(json, v.ext, "byteLength", "1");
                 }),
                 "compressed bytes");
    expect_error(load_edited([](std::string& json, const PositionView& v) {
                     set_number(json, v.ext, "count", "10000000");
                     set_number(json, v.begin, "byteLength", "120000000");
                 }),
                 "compressed bytes");
    // Two views claiming 900 MB each pass every per-view size check but
    // exceed the asset's 1 GiB decoded total, which is checked before any
    // view is allocated or decoded.
    expect_error(load_edited([](std::string& json, const PositionView& v) {
                     set_number(json, v.ext, "count", "75000000");
                     set_number(json, v.begin, "byteLength", "900000000");
                     // The view object ends where its extension objects close.
                     const std::size_t end = json.find("}}}", v.begin) + 3;
                     json.insert(end, "," + json.substr(v.begin, end - v.begin));
                 }),
                 "asset's limit");

    // Garbage in place of the meshopt streams: decode must fail cleanly.
    write_file(out, original.data(), original.size());
    const fs::path bin = dir.path() / "cooked.bin";
    std::string bytes = read_text(bin);
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(0xA5 ^ (i * 7));
    write_file(bin, bytes.data(), bytes.size());
    EXPECT_ERR(GltfLoader::load_mesh_data(out));
}
