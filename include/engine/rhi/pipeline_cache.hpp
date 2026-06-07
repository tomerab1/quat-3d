#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <unordered_map>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::rhi {

class Device;

// A SPIR-V shader module plus the reflection parsed from its sibling .json.
struct LoadedShader {
    VkShaderModule   module = VK_NULL_HANDLE;
    ShaderReflection reflection;
};

// Loads and caches SPIR-V shader modules by source path, and owns the Vulkan
// VkPipelineCache used when pipelines are created (Slice 2.5). Move-only; owns
// all cached VkShaderModules.
class PipelineCache {
public:
    [[nodiscard]] static std::expected<PipelineCache, core::Error> create(const Device& device);

    PipelineCache() = default;
    ~PipelineCache();
    PipelineCache(PipelineCache&& other) noexcept;
    PipelineCache& operator=(PipelineCache&& other) noexcept;
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    [[nodiscard]] VkPipelineCache handle() const { return cache_; }

    // Loads `spv_path` (and its sibling reflection .json), creating and caching
    // the shader module. Repeated calls for the same path return the cached
    // entry. The returned pointer is stable for the cache's lifetime.
    [[nodiscard]] std::expected<const LoadedShader*, core::Error>
    load(const std::string& spv_path);

private:
    void destroy() noexcept;

    VkDevice        device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_  = VK_NULL_HANDLE;
    std::unordered_map<std::size_t, LoadedShader> modules_;
};

} // namespace engine::rhi
