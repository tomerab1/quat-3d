// Parses Slang `-reflection-json` output into engine reflection structures.
//
// JSON_NOEXCEPTION turns nlohmann's internal throws into std::abort (we build
// with -fno-exceptions). All access below is guarded with type checks and
// .value() defaults so a malformed file is rejected via std::expected rather
// than aborting.
#define JSON_NOEXCEPTION

#include "engine/rhi/shader_reflection.hpp"

#include <algorithm>
#include <string>
#include <array>

#include <nlohmann/json.hpp>

namespace engine::rhi {

namespace {

using json = nlohmann::json;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] VkShaderStageFlagBits stage_from_string(std::string_view s) {
    if (s == "vertex")       return VK_SHADER_STAGE_VERTEX_BIT;
    if (s == "fragment" || s == "pixel") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (s == "compute")      return VK_SHADER_STAGE_COMPUTE_BIT;
    if (s == "geometry")     return VK_SHADER_STAGE_GEOMETRY_BIT;
    if (s == "hull")         return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (s == "domain")       return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (s == "amplification")return VK_SHADER_STAGE_TASK_BIT_EXT;
    if (s == "mesh")         return VK_SHADER_STAGE_MESH_BIT_EXT;
    return VK_SHADER_STAGE_VERTEX_BIT;
}

// Maps a parameter's Slang `type` object to a Vulkan descriptor type.
[[nodiscard]] VkDescriptorType descriptor_type_from_type(const json& type) {
    const std::string kind = type.value("kind", std::string{});
    if (kind == "constantBuffer") return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (kind == "samplerState")   return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (kind == "resource") {
        const std::string shape  = type.value("baseShape", std::string{});
        const std::string access = type.value("access", std::string{"read"});
        if (shape == "structuredBuffer" || shape == "byteAddressBuffer") {
            return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        }
        if (shape.rfind("texture", 0) == 0) {
            return access == "readWrite" ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                         : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}

// Size of a push-constant block = max(offset + size) over its leaf fields.
[[nodiscard]] std::uint32_t push_constant_size(const json& param) {
    if (!param.contains("type")) return 0;
    const json& type = param["type"];
    if (!type.contains("elementType")) return 0;
    const json& element = type["elementType"];
    if (!element.contains("fields") || !element["fields"].is_array()) return 0;

    std::uint32_t end = 0;
    for (const json& field : element["fields"]) {
        if (!field.contains("binding")) continue;
        const json& b = field["binding"];
        if (b.value("kind", std::string{}) != "uniform") continue;
        const std::uint32_t offset = b.value("offset", 0U);
        const std::uint32_t size   = b.value("size", 0U);
        end = std::max(end, offset + size);
    }
    return end;
}

// SPIR-V ImageFormat (spec 3.11) -> VkFormat; index = enum value.
constexpr std::array<VkFormat, 42> kSpirvImageFormats{
    VK_FORMAT_UNDEFINED,                // Unknown
    VK_FORMAT_R32G32B32A32_SFLOAT,      // Rgba32f
    VK_FORMAT_R16G16B16A16_SFLOAT,      // Rgba16f
    VK_FORMAT_R32_SFLOAT,               // R32f
    VK_FORMAT_R8G8B8A8_UNORM,           // Rgba8
    VK_FORMAT_R8G8B8A8_SNORM,           // Rgba8Snorm
    VK_FORMAT_R32G32_SFLOAT,            // Rg32f
    VK_FORMAT_R16G16_SFLOAT,            // Rg16f
    VK_FORMAT_B10G11R11_UFLOAT_PACK32,  // R11fG11fB10f
    VK_FORMAT_R16_SFLOAT,               // R16f
    VK_FORMAT_R16G16B16A16_UNORM,       // Rgba16
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, // Rgb10A2
    VK_FORMAT_R16G16_UNORM,             // Rg16
    VK_FORMAT_R8G8_UNORM,               // Rg8
    VK_FORMAT_R16_UNORM,                // R16
    VK_FORMAT_R8_UNORM,                 // R8
    VK_FORMAT_R16G16B16A16_SNORM,       // Rgba16Snorm
    VK_FORMAT_R16G16_SNORM,             // Rg16Snorm
    VK_FORMAT_R8G8_SNORM,               // Rg8Snorm
    VK_FORMAT_R16_SNORM,                // R16Snorm
    VK_FORMAT_R8_SNORM,                 // R8Snorm
    VK_FORMAT_R32G32B32A32_SINT,        // Rgba32i
    VK_FORMAT_R16G16B16A16_SINT,        // Rgba16i
    VK_FORMAT_R8G8B8A8_SINT,            // Rgba8i
    VK_FORMAT_R32_SINT,                 // R32i
    VK_FORMAT_R32G32_SINT,              // Rg32i
    VK_FORMAT_R16G16_SINT,              // Rg16i
    VK_FORMAT_R8G8_SINT,                // Rg8i
    VK_FORMAT_R16_SINT,                 // R16i
    VK_FORMAT_R8_SINT,                  // R8i
    VK_FORMAT_R32G32B32A32_UINT,        // Rgba32ui
    VK_FORMAT_R16G16B16A16_UINT,        // Rgba16ui
    VK_FORMAT_R8G8B8A8_UINT,            // Rgba8ui
    VK_FORMAT_R32_UINT,                 // R32ui
    VK_FORMAT_A2B10G10R10_UINT_PACK32,  // Rgb10a2ui
    VK_FORMAT_R32G32_UINT,              // Rg32ui
    VK_FORMAT_R16G16_UINT,              // Rg16ui
    VK_FORMAT_R8G8_UINT,                // Rg8ui
    VK_FORMAT_R16_UINT,                 // R16ui
    VK_FORMAT_R8_UINT,                  // R8ui
    VK_FORMAT_R64_UINT,                 // R64ui
    VK_FORMAT_R64_SINT,                 // R64i
};

} // namespace

std::vector<VkFormat> spirv_storage_image_formats(std::span<const std::uint32_t> spirv) {
    constexpr std::uint32_t kMagic = 0x07230203U;
    constexpr std::uint32_t kOpTypeImage = 25U;
    constexpr std::uint32_t kDimBuffer = 5U;
    constexpr std::uint32_t kDimSubpassData = 6U;
    constexpr std::uint32_t kSampledStorage = 2U;
    constexpr std::size_t kHeaderWords = 5;

    std::vector<VkFormat> out;
    if (spirv.size() < kHeaderWords || spirv[0] != kMagic) return out;
    for (std::size_t i = kHeaderWords; i < spirv.size();) {
        const std::uint32_t word_count = spirv[i] >> 16U;
        const std::uint32_t opcode = spirv[i] & 0xFFFFU;
        if (word_count == 0 || i + word_count > spirv.size()) return {}; // malformed
        // OpTypeImage: result, sampled type, Dim, Depth, Arrayed, MS, Sampled,
        // Image Format [, access qualifier].
        if (opcode == kOpTypeImage && word_count >= 9) {
            const std::uint32_t dim = spirv[i + 3];
            const std::uint32_t sampled = spirv[i + 7];
            const std::uint32_t format = spirv[i + 8];
            if (sampled == kSampledStorage && dim != kDimBuffer && dim != kDimSubpassData) {
                // An unmapped (future) format is reported as undefined: the
                // caller then refuses it rather than guessing.
                out.push_back(format < kSpirvImageFormats.size() ? kSpirvImageFormats[format]
                                                                  : VK_FORMAT_UNDEFINED);
            }
        }
        i += word_count;
    }
    return out;
}

std::expected<ShaderReflection, core::Error>
parse_reflection_json(std::string_view json_text) {
    const json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (root.is_discarded()) {
        return fail("reflection JSON parse error");
    }

    ShaderReflection refl;

    if (root.contains("entryPoints") && root["entryPoints"].is_array()) {
        for (const json& e : root["entryPoints"]) {
            EntryPoint ep;
            ep.name  = e.value("name", std::string{});
            ep.stage = stage_from_string(e.value("stage", std::string{}));
            refl.entry_points.push_back(std::move(ep));
        }
    }

    if (root.contains("parameters") && root["parameters"].is_array()) {
        for (const json& p : root["parameters"]) {
            if (!p.contains("binding")) continue;
            const json& binding = p["binding"];
            const std::string kind = binding.value("kind", std::string{});

            if (kind == "pushConstantBuffer") {
                refl.push_constant_ranges.push_back(
                    PushConstantRange{0, push_constant_size(p), VK_SHADER_STAGE_ALL});
            } else if (kind == "descriptorTableSlot") {
                DescriptorBinding db;
                db.set     = binding.value("space", 0U);
                db.binding = binding.value("index", 0U);
                db.count   = binding.value("count", 1U);
                db.stages  = VK_SHADER_STAGE_ALL;
                db.type    = p.contains("type") ? descriptor_type_from_type(p["type"])
                                                : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                refl.descriptor_bindings.push_back(db);
            }
        }
    }

    return refl;
}

namespace {

[[nodiscard]] std::uint32_t push_constant_end(const ShaderReflection& r) {
    std::uint32_t end = 0;
    for (const PushConstantRange& pc : r.push_constant_ranges) {
        end = std::max(end, pc.offset + pc.size);
    }
    return end;
}

[[nodiscard]] std::string binding_name(const DescriptorBinding& b) {
    return "set " + std::to_string(b.set) + " binding " + std::to_string(b.binding);
}

} // namespace

std::expected<void, core::Error>
check_reflection_compatible(const ShaderReflection& baseline, const ShaderReflection& updated,
                            std::span<const std::string> required_entries) {
    for (const std::string& entry : required_entries) {
        const bool found = std::any_of(updated.entry_points.begin(), updated.entry_points.end(),
                                       [&](const EntryPoint& e) { return e.name == entry; });
        if (!found) return fail("entry point '" + entry + "' no longer exists");
    }
    for (const DescriptorBinding& b : updated.descriptor_bindings) {
        const auto it = std::find_if(
            baseline.descriptor_bindings.begin(), baseline.descriptor_bindings.end(),
            [&](const DescriptorBinding& o) { return o.set == b.set && o.binding == b.binding; });
        if (it == baseline.descriptor_bindings.end()) {
            return fail(binding_name(b) + " is new (the pipeline layout has no such binding)");
        }
        if (it->type != b.type) {
            return fail(binding_name(b) + " changed descriptor type");
        }
        if (b.count > it->count) {
            return fail(binding_name(b) + " array grew from " + std::to_string(it->count) +
                        " to " + std::to_string(b.count));
        }
    }
    const std::uint32_t old_pc = push_constant_end(baseline);
    const std::uint32_t new_pc = push_constant_end(updated);
    if (new_pc > old_pc) {
        return fail("push-constant block grew from " + std::to_string(old_pc) + " to " +
                    std::to_string(new_pc) + " bytes");
    }
    return {};
}

} // namespace engine::rhi
