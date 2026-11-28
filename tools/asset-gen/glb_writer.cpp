#include "asset-gen/glb_writer.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

namespace quat::asset_gen {
namespace {

constexpr std::uint32_t k_glb_magic = 0x46546C67U; // "glTF"
constexpr std::uint32_t k_chunk_json = 0x4E4F534AU;
constexpr std::uint32_t k_chunk_bin = 0x004E4942U;
constexpr int k_float = 5126;
constexpr int k_ushort = 5123;
constexpr int k_uint = 5125;
constexpr int k_array_buffer = 34962;
constexpr int k_element_array_buffer = 34963;

void put_u32(std::vector<std::byte>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::byte>((v >> (8 * i)) & 0xFFU));
}

void put_f32(std::vector<std::byte>& out, float f) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    put_u32(out, bits);
}

void pad_to_4(std::vector<std::byte>& out, std::byte fill) {
    while (out.size() % 4 != 0) out.push_back(fill);
}

// Appends a buffer view of `bin[offset, end)` and returns its index.
std::size_t add_view(nlohmann::json& views, std::size_t offset, std::size_t end, int target) {
    views.push_back({{"buffer", 0},
                     {"byteOffset", offset},
                     {"byteLength", end - offset},
                     {"target", target}});
    return views.size() - 1;
}

nlohmann::json vec3_json(const glm::vec3& v) {
    return nlohmann::json::array({static_cast<double>(v.x), static_cast<double>(v.y),
                                  static_cast<double>(v.z)});
}

} // namespace

std::vector<std::byte> write_glb(const Model& model) {
    nlohmann::json views = nlohmann::json::array();
    nlohmann::json accessors = nlohmann::json::array();
    nlohmann::json prims = nlohmann::json::array();
    std::vector<std::byte> bin;

    for (const Primitive& p : model.primitives) {
        if (p.indices.empty()) continue;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());

        std::size_t start = bin.size();
        for (const glm::vec3& v : p.positions) {
            put_f32(bin, v.x);
            put_f32(bin, v.y);
            put_f32(bin, v.z);
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
        const std::size_t pos_view = add_view(views, start, bin.size(), k_array_buffer);
        accessors.push_back({{"bufferView", pos_view},
                             {"componentType", k_float},
                             {"count", p.positions.size()},
                             {"type", "VEC3"},
                             {"min", vec3_json(lo)},
                             {"max", vec3_json(hi)}});
        const std::size_t pos_acc = accessors.size() - 1;

        start = bin.size();
        for (const glm::vec3& n : p.normals) {
            put_f32(bin, n.x);
            put_f32(bin, n.y);
            put_f32(bin, n.z);
        }
        const std::size_t nrm_view = add_view(views, start, bin.size(), k_array_buffer);
        accessors.push_back({{"bufferView", nrm_view},
                             {"componentType", k_float},
                             {"count", p.normals.size()},
                             {"type", "VEC3"}});
        const std::size_t nrm_acc = accessors.size() - 1;

        const bool narrow = p.positions.size() <= 0xFFFFU;
        start = bin.size();
        for (const std::uint32_t i : p.indices) {
            if (narrow) {
                bin.push_back(static_cast<std::byte>(i & 0xFFU));
                bin.push_back(static_cast<std::byte>((i >> 8U) & 0xFFU));
            } else {
                put_u32(bin, i);
            }
        }
        const std::size_t idx_view = add_view(views, start, bin.size(), k_element_array_buffer);
        pad_to_4(bin, std::byte{0});
        accessors.push_back({{"bufferView", idx_view},
                             {"componentType", narrow ? k_ushort : k_uint},
                             {"count", p.indices.size()},
                             {"type", "SCALAR"}});
        const std::size_t idx_acc = accessors.size() - 1;

        prims.push_back({{"attributes", {{"POSITION", pos_acc}, {"NORMAL", nrm_acc}}},
                         {"indices", idx_acc},
                         {"material", p.material},
                         {"mode", 4}});
    }

    nlohmann::json materials = nlohmann::json::array();
    for (const Material& m : model.materials) {
        materials.push_back(
            {{"name", m.name},
             {"pbrMetallicRoughness",
              {{"baseColorFactor",
                {static_cast<double>(m.base_color.r), static_cast<double>(m.base_color.g),
                 static_cast<double>(m.base_color.b), static_cast<double>(m.base_color.a)}},
               {"metallicFactor", static_cast<double>(m.metallic)},
               {"roughnessFactor", static_cast<double>(m.roughness)}}}});
    }

    nlohmann::json doc;
    doc["asset"] = {{"version", "2.0"}, {"generator", "quat-asset-gen"}};
    doc["scene"] = 0;
    doc["scenes"] = nlohmann::json::array({{{"nodes", {0}}}});
    doc["nodes"] = nlohmann::json::array({{{"name", model.name}, {"mesh", 0}}});
    doc["meshes"] = nlohmann::json::array({{{"name", model.name}, {"primitives", prims}}});
    doc["materials"] = materials;
    doc["accessors"] = accessors;
    doc["bufferViews"] = views;
    doc["buffers"] = nlohmann::json::array({{{"byteLength", bin.size()}}});

    const std::string json = doc.dump();
    std::vector<std::byte> json_chunk(json.size());
    std::memcpy(json_chunk.data(), json.data(), json.size());
    pad_to_4(json_chunk, std::byte{' '});
    pad_to_4(bin, std::byte{0});

    std::vector<std::byte> out;
    out.reserve(12 + 8 + json_chunk.size() + 8 + bin.size());
    put_u32(out, k_glb_magic);
    put_u32(out, 2);
    put_u32(out, static_cast<std::uint32_t>(12 + 8 + json_chunk.size() + 8 + bin.size()));
    put_u32(out, static_cast<std::uint32_t>(json_chunk.size()));
    put_u32(out, k_chunk_json);
    out.insert(out.end(), json_chunk.begin(), json_chunk.end());
    put_u32(out, static_cast<std::uint32_t>(bin.size()));
    put_u32(out, k_chunk_bin);
    out.insert(out.end(), bin.begin(), bin.end());
    return out;
}

} // namespace quat::asset_gen
