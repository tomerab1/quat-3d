#pragma once

#include <cstdint>
#include <expected>
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
};

// Result of parsing a Slang `-reflection-json` file: entry points plus the
// resource interface (push constants + descriptor bindings) the pipeline layout
// is derived from. Stage visibility is reported as VK_SHADER_STAGE_ALL since the
// global reflection does not attribute resources to individual stages.
struct ShaderReflection {
    std::vector<EntryPoint>        entry_points;
    std::vector<PushConstantRange> push_constant_ranges;
    std::vector<DescriptorBinding> descriptor_bindings;
};

[[nodiscard]] std::expected<ShaderReflection, core::Error>
parse_reflection_json(std::string_view json_text);

} // namespace engine::rhi
