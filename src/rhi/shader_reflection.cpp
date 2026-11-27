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

} // namespace

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
