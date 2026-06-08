#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

class Device;
struct LoadedShader;

// A compute VkPipeline plus the VkPipelineLayout it was built with. Move-only
// RAII. Used by compute passes (e.g. the deferred lighting pass). When set
// layouts are supplied the pipeline is created with the descriptor-buffer bit.
class ComputePipeline {
public:
    struct CreateInfo {
        const LoadedShader* shader = nullptr;
        const char*         entry  = "compute_main";
        std::span<const VkDescriptorSetLayout> set_layouts{};
        std::span<const VkPushConstantRange>   push_constants{};
    };

    [[nodiscard]] static std::expected<ComputePipeline, core::Error>
    create(const Device& device, VkPipelineCache cache, const CreateInfo& info);

    ComputePipeline() = default;
    ~ComputePipeline();
    ComputePipeline(ComputePipeline&& other) noexcept;
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    void destroy() noexcept;

    VkDevice         device_   = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
};

} // namespace engine::rhi
