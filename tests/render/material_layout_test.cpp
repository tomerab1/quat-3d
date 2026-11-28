// The shader side of the material contract matches the C++ side (R12.11
// review): lib/material.slang is the one shader-side copy of the flag bits
// and of PbrMaterialParams, and every pass reading a material's constants
// (transparent.slang, shadow_material.slang) sees the C++ std140 layout.
// Flags are read from the module source; layouts from the cooked reflection.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "engine/asset/material_asset.hpp"

namespace {

namespace a = engine::asset;
namespace fs = std::filesystem;
using nlohmann::json;

std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

TEST(MaterialContract, FlagBitsMatchTheCppEnum) {
    const fs::path src = fs::path(QUAT_SHADER_SOURCE_DIR) / "lib" / "material.slang";
    const std::string text = read_text(src);
    ASSERT_FALSE(text.empty()) << src;
    std::map<std::string, std::uint32_t> shader;
    const std::regex bit(R"(MATERIAL_([A-Z_]+)\s*=\s*1u\s*<<\s*(\d+))");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), bit); it != std::sregex_iterator();
         ++it) {
        shader[(*it)[1].str()] = 1U << std::stoul((*it)[2].str());
    }
    const std::map<std::string, std::uint32_t> cpp{
        {"HAS_BASE_COLOR", a::material_has_base_color},
        {"HAS_NORMAL", a::material_has_normal},
        {"HAS_METALLIC_ROUGHNESS", a::material_has_metallic_roughness},
        {"HAS_EMISSIVE", a::material_has_emissive},
        {"HAS_OCCLUSION", a::material_has_occlusion},
        {"BLEND", a::material_blend},
        {"TRANSMISSION", a::material_transmission},
        {"DOUBLE_SIDED", a::material_double_sided},
        {"HAS_THICKNESS", a::material_has_thickness},
        {"ALPHA_MASK", a::material_alpha_mask},
    };
    EXPECT_EQ(shader, cpp);
}

// Every field of the `g_params` constant buffer of `shader` sits at the C++
// offset of the field of the same name.
void expect_params_layout(const char* shader) {
    const fs::path path = fs::path(QUAT_COOKED_SHADER_DIR) / (std::string(shader) + ".json");
    const json root = json::parse(read_text(path), nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(root.is_discarded()) << path;
    const std::map<std::string, std::size_t> cpp{
        {"base_color_factor", offsetof(a::PbrMaterialParams, base_color_factor)},
        {"emissive_factor", offsetof(a::PbrMaterialParams, emissive_factor)},
        {"metallic_factor", offsetof(a::PbrMaterialParams, metallic_factor)},
        {"roughness_factor", offsetof(a::PbrMaterialParams, roughness_factor)},
        {"occlusion_strength", offsetof(a::PbrMaterialParams, occlusion_strength)},
        {"normal_scale", offsetof(a::PbrMaterialParams, normal_scale)},
        {"alpha_cutoff", offsetof(a::PbrMaterialParams, alpha_cutoff)},
        {"flags", offsetof(a::PbrMaterialParams, flags)},
        {"transmission_factor", offsetof(a::PbrMaterialParams, transmission_factor)},
        {"ior", offsetof(a::PbrMaterialParams, ior)},
        {"attenuation", offsetof(a::PbrMaterialParams, attenuation)},
        {"thickness", offsetof(a::PbrMaterialParams, thickness)},
        {"clearcoat_factor", offsetof(a::PbrMaterialParams, clearcoat_factor)},
        {"clearcoat_roughness", offsetof(a::PbrMaterialParams, clearcoat_roughness)},
        {"pad", offsetof(a::PbrMaterialParams, pad)},
        {"uv_mat", offsetof(a::PbrMaterialParams, uv_mat)},
        {"uv_offset", offsetof(a::PbrMaterialParams, uv_offset)},
        {"shader_params", offsetof(a::PbrMaterialParams, shader_params)},
    };
    // A path of object keys into `j`, or null where it is not there.
    const auto at = [](const json* j, std::initializer_list<const char*> keys) -> const json* {
        for (const char* k : keys) {
            if (j == nullptr || !j->is_object() || !j->contains(k)) return nullptr;
            j = &(*j)[k];
        }
        return j;
    };
    const json* fields = nullptr;
    if (const json* params = at(&root, {"parameters"}); params != nullptr && params->is_array()) {
        for (const json& p : *params) {
            const json* name = at(&p, {"name"});
            if (name != nullptr && name->is_string() && *name == "g_params") {
                fields = at(&p, {"type", "elementType", "fields"});
            }
        }
    }
    ASSERT_NE(fields, nullptr) << shader << " has no g_params fields";
    ASSERT_TRUE(fields->is_array());
    EXPECT_EQ(fields->size(), cpp.size()) << shader;
    for (const json& f : *fields) {
        const json* name = at(&f, {"name"});
        const json* offset = at(&f, {"binding", "offset"});
        ASSERT_TRUE(name != nullptr && name->is_string() && offset != nullptr &&
                    offset->is_number_unsigned())
            << shader;
        const auto it = cpp.find(name->get_ref<const std::string&>());
        ASSERT_NE(it, cpp.end()) << shader << ": unknown field " << *name;
        EXPECT_EQ(offset->get<std::size_t>(), it->second) << shader << ": " << *name;
    }
}

TEST(MaterialContract, ParamsLayoutMatchesInEveryPass) {
    if (!fs::is_directory(QUAT_COOKED_SHADER_DIR)) {
        GTEST_SKIP() << "no cooked shaders at " << QUAT_COOKED_SHADER_DIR;
    }
    expect_params_layout("transparent");
    expect_params_layout("shadow_material");
}

} // namespace
