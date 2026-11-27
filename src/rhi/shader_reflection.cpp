// Parses Slang `-reflection-json` output into engine reflection structures.
//
// The JSON comes from files on disk (cooked at build time, rewritten by shader
// hot reload at run time), so it is untrusted: every field is read through
// core::JsonFields, which type- and range-checks it. Malformed input is
// rejected with a precise error, never an abort (nlohmann's get()/value()
// abort on a type mismatch in this exception-free build).
#define JSON_NOEXCEPTION

#include "engine/rhi/shader_reflection.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/core/json_fields.hpp"

namespace engine::rhi {

namespace {

using json = nlohmann::json;
using core::JsonFields;

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Limits on what a reflection file may declare (well above any real shader,
// well below anything that overflows the layout arithmetic).
constexpr std::int64_t k_max_set = 63;
constexpr std::int64_t k_max_binding = 65535;
constexpr std::int64_t k_max_count = 1 << 20;
constexpr std::int64_t k_max_push_bytes = 1 << 16;

[[nodiscard]] std::optional<VkShaderStageFlagBits> stage_from_string(std::string_view s) {
    if (s == "vertex")       return VK_SHADER_STAGE_VERTEX_BIT;
    if (s == "fragment" || s == "pixel") return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (s == "compute")      return VK_SHADER_STAGE_COMPUTE_BIT;
    if (s == "geometry")     return VK_SHADER_STAGE_GEOMETRY_BIT;
    if (s == "hull")         return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (s == "domain")       return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    if (s == "amplification")return VK_SHADER_STAGE_TASK_BIT_EXT;
    if (s == "mesh")         return VK_SHADER_STAGE_MESH_BIT_EXT;
    return std::nullopt;
}

[[nodiscard]] std::string element(const std::string& context, std::size_t i) {
    return context + "[" + std::to_string(i) + "]";
}

// Maps a parameter's Slang `type` object to a Vulkan descriptor type.
[[nodiscard]] std::expected<VkDescriptorType, core::Error>
descriptor_type_from_type(const json& type, const std::string& context) {
    JsonFields f(type, context, /*strict=*/false);
    std::string kind;
    std::string shape;
    std::string access = "read";
    f.string("kind", kind).string("baseShape", shape).string("access", access);
    if (auto ok = f.finish(); !ok) return std::unexpected(ok.error());
    if (kind == "constantBuffer") return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    if (kind == "samplerState")   return VK_DESCRIPTOR_TYPE_SAMPLER;
    if (kind == "resource") {
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

// Size of a push-constant block = max(offset + size) over its leaf fields
// (param.type.elementType.fields[].binding, kind "uniform").
[[nodiscard]] std::expected<std::uint32_t, core::Error> push_constant_size(JsonFields& param,
                                                                         const std::string& context) {
    const json* type = param.raw("type");
    if (type == nullptr || !type->is_object()) return 0;
    const auto element_it = type->find("elementType");
    if (element_it == type->end() || !element_it->is_object()) return 0;
    const auto fields_it = element_it->find("fields");
    if (fields_it == element_it->end()) return 0;
    const std::string fields_context = context + ".type.elementType.fields";
    if (!fields_it->is_array()) return fail(fields_context + ": expected an array");

    std::int64_t end = 0;
    for (std::size_t i = 0; i < fields_it->size(); ++i) {
        const std::string field_context = element(fields_context, i);
        JsonFields field((*fields_it)[i], field_context, /*strict=*/false);
        const json* binding = field.raw("binding");
        if (auto ok = field.finish(); !ok) return std::unexpected(ok.error());
        if (binding == nullptr) continue;
        JsonFields b(*binding, field_context + ".binding", /*strict=*/false);
        std::string kind;
        b.string("kind", kind);
        std::int64_t offset = 0;
        std::int64_t size = 0;
        if (kind == "uniform") {
            b.integer("offset", offset, 0, k_max_push_bytes).integer("size", size, 0, k_max_push_bytes);
        }
        if (auto ok = b.finish(); !ok) return std::unexpected(ok.error());
        end = std::max(end, offset + size);
    }
    if (end > k_max_push_bytes) {
        return fail(context + ": push-constant block of " + std::to_string(end) +
                    " bytes is larger than " + std::to_string(k_max_push_bytes));
    }
    return static_cast<std::uint32_t>(end);
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
    JsonFields fields(root, "reflection", /*strict=*/false);
    const json* entry_points = fields.raw("entryPoints");
    const json* parameters = fields.raw("parameters");
    if (auto ok = fields.finish(); !ok) return std::unexpected(ok.error());

    ShaderReflection refl;

    if (entry_points != nullptr) {
        if (!entry_points->is_array()) return fail("reflection.entryPoints: expected an array");
        for (std::size_t i = 0; i < entry_points->size(); ++i) {
            const std::string context = element("reflection.entryPoints", i);
            JsonFields e((*entry_points)[i], context, /*strict=*/false);
            EntryPoint ep;
            std::string stage;
            e.string("name", ep.name).string("stage", stage);
            if (auto ok = e.finish(); !ok) return std::unexpected(ok.error());
            const auto bit = stage_from_string(stage);
            if (!bit) return fail(context + ".stage: unknown stage '" + stage + "'");
            ep.stage = *bit;
            refl.entry_points.push_back(std::move(ep));
        }
    }

    if (parameters != nullptr) {
        if (!parameters->is_array()) return fail("reflection.parameters: expected an array");
        for (std::size_t i = 0; i < parameters->size(); ++i) {
            const std::string context = element("reflection.parameters", i);
            JsonFields p((*parameters)[i], context, /*strict=*/false);
            const json* binding = p.raw("binding");
            if (auto ok = p.finish(); !ok) return std::unexpected(ok.error());
            if (binding == nullptr) continue;

            JsonFields b(*binding, context + ".binding", /*strict=*/false);
            std::string kind;
            b.string("kind", kind);
            if (kind == "pushConstantBuffer") {
                if (auto ok = b.finish(); !ok) return std::unexpected(ok.error());
                auto size = push_constant_size(p, context);
                if (!size) return std::unexpected(size.error());
                refl.push_constant_ranges.push_back(
                    PushConstantRange{0, *size, VK_SHADER_STAGE_ALL});
            } else if (kind == "descriptorTableSlot") {
                DescriptorBinding db;
                b.integer("space", db.set, 0, k_max_set)
                    .integer("index", db.binding, 0, k_max_binding)
                    .integer("count", db.count, 1, k_max_count);
                if (auto ok = b.finish(); !ok) return std::unexpected(ok.error());
                db.stages = VK_SHADER_STAGE_ALL;
                p.string("name", db.name, 256);
                if (const json* type = p.raw("type")) {
                    auto t = descriptor_type_from_type(*type, context + ".type");
                    if (!t) return std::unexpected(t.error());
                    db.type = *t;
                }
                if (auto ok = p.finish(); !ok) return std::unexpected(ok.error());
                refl.descriptor_bindings.push_back(std::move(db));
            } else if (auto ok = b.finish(); !ok) {
                return std::unexpected(ok.error());
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

const DescriptorBinding* ShaderReflection::find_binding(std::string_view name) const {
    const auto it = std::find_if(descriptor_bindings.begin(), descriptor_bindings.end(),
                                 [&](const DescriptorBinding& b) { return b.name == name; });
    return it == descriptor_bindings.end() ? nullptr : &*it;
}

std::expected<std::vector<DescriptorBinding>, core::Error>
reflected_bindings(const ShaderReflection& reflection, std::span<const BindingRequest> requests,
                   VkShaderStageFlags stages) {
    std::vector<DescriptorBinding> out;
    out.reserve(requests.size());
    for (const BindingRequest& request : requests) {
        const DescriptorBinding* b = reflection.find_binding(request.name);
        const std::string name(request.name);
        if (b == nullptr) return fail("shader declares no resource '" + name + "'");
        if (b->set != 0) return fail("'" + name + "' is in set " + std::to_string(b->set) +
                                     " (the pass binds set 0)");
        if (b->type != request.type) {
            return fail("'" + name + "' has descriptor type " + std::to_string(b->type) +
                        ", the pass binds " + std::to_string(request.type));
        }
        DescriptorBinding binding = *b;
        binding.stages = stages;
        out.push_back(std::move(binding));
    }
    return out;
}

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
        if (!it->name.empty() && !b.name.empty() && it->name != b.name) {
            return fail(binding_name(b) + " was '" + it->name + "', now '" + b.name + "'");
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
