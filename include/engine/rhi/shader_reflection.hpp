#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

struct EntryPoint {
    std::string         name;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
};

struct PushConstantRange {
    std::uint32_t      offset = 0;
    std::uint32_t      size = 0;
    VkShaderStageFlags stages = VK_SHADER_STAGE_ALL;
};

struct DescriptorBinding {
    std::uint32_t      set = 0;
    std::uint32_t      binding = 0;
    VkDescriptorType   type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    std::uint32_t      count = 1;
    VkShaderStageFlags stages = VK_SHADER_STAGE_ALL;
    std::string        name{}; // shader parameter name (from reflection; empty if hand-written)
};

// Result of parsing a Slang `-reflection-json` file: entry points plus the
// resource interface (push constants + descriptor bindings) the pipeline layout
// is derived from. Stage visibility is reported as VK_SHADER_STAGE_ALL since the
// global reflection does not attribute resources to individual stages.
struct ShaderReflection {
    std::vector<EntryPoint>        entry_points;
    std::vector<PushConstantRange> push_constant_ranges;
    std::vector<DescriptorBinding> descriptor_bindings;

    // The binding of the shader parameter `name`, or null.
    [[nodiscard]] const DescriptorBinding* find_binding(std::string_view name) const;
};

[[nodiscard]] std::expected<ShaderReflection, core::Error>
parse_reflection_json(std::string_view json_text);

// A resource a pass binds, named as the shader declares it, with the
// descriptor type the pass writes for it.
struct BindingRequest {
    std::string_view name;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
};

// Derives a pass's descriptor-set layout from reflection instead of
// hard-coded indices: each request is looked up by name and must be a
// set-0 binding of the requested type. Returns the bindings in request order
// with `stages` set (ready for DescriptorSetLayout::create; `.binding` is the
// index to write each descriptor at). The error names the shader parameter
// that is missing, in another set, or of another type.
[[nodiscard]] std::expected<std::vector<DescriptorBinding>, core::Error>
reflected_bindings(const ShaderReflection& reflection, std::span<const BindingRequest> requests,
                   VkShaderStageFlags stages);

// Whether a recompiled shader (`updated`) still fits the pipeline layout that
// was built for `baseline` (shader hot reload keeps the layout; ROADMAP R2):
//   * every entry point in `required_entries` still exists;
//   * every descriptor binding of `updated` exists in `baseline` with the same
//     descriptor type and no larger array count (removing bindings is fine);
//   * a binding slot still holds the same named parameter (passes write their
//     descriptors at the slots reflection gave each name at creation);
//   * the push-constant block did not grow.
// The error names the first incompatibility found.
[[nodiscard]] std::expected<void, core::Error>
check_reflection_compatible(const ShaderReflection& baseline, const ShaderReflection& updated,
                            std::span<const std::string> required_entries);
// The image format of every storage image (OpTypeImage with Sampled = 2,
// texel buffers and subpass inputs excluded) a SPIR-V module declares, as a
// VkFormat; VK_FORMAT_UNDEFINED marks one declared without a format (Slang:
// RWTexture without [format(...)]), which needs the optional
// shaderStorageImage{Read,Write}WithoutFormat features. The Slang JSON
// reflection does not carry image formats, hence the SPIR-V walk. A
// malformed module yields an empty list (vkCreateShaderModule rejects it).
[[nodiscard]] std::vector<VkFormat> spirv_storage_image_formats(std::span<const std::uint32_t> spirv);

} // namespace engine::rhi
