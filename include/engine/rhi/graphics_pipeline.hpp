#pragma once

#include <cstdint>
#include <expected>
#include <span>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

class Device;
struct LoadedShader;

// A graphics VkPipeline plus the VkPipelineLayout it was built with, configured
// for dynamic rendering (no VkRenderPass/VkFramebuffer). Move-only RAII.
//
// Slice 2.5 only needs a vertex-buffer-less, descriptor-less pipeline; the
// layout is derived from the shaders' reflected push-constant ranges (none yet).
// Vertex input, descriptor set layouts, and depth state grow in later phases.
class GraphicsPipeline {
public:
    struct CreateInfo {
        // Shader modules. A single module may host both entry points (as the
        // hard-coded triangle does), so the same LoadedShader can be passed twice.
        const LoadedShader* vertex   = nullptr;
        const LoadedShader* fragment = nullptr;
        const char*         vertex_entry   = "vertex_main";
        const char*         fragment_entry = "fragment_main";
        // Colour attachment formats for the dynamic-rendering pipeline. Must
        // match the formats the pass renders into.
        std::span<const VkFormat> color_formats{};
        VkFormat                  depth_format = VK_FORMAT_UNDEFINED;
        VkCullModeFlags           cull_mode  = VK_CULL_MODE_NONE;
        VkFrontFace               front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    };

    [[nodiscard]] static std::expected<GraphicsPipeline, core::Error>
    create(const Device& device, VkPipelineCache cache, const CreateInfo& info);

    GraphicsPipeline() = default;
    ~GraphicsPipeline();
    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const { return pipeline_; }
    [[nodiscard]] VkPipelineLayout layout() const { return layout_; }

private:
    void destroy() noexcept;

    VkDevice         device_   = VK_NULL_HANDLE;
    VkPipelineLayout layout_   = VK_NULL_HANDLE;
    VkPipeline       pipeline_ = VK_NULL_HANDLE;
};

} // namespace engine::rhi
