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
        // match the formats the pass renders into. One disabled blend attachment
        // is created per format (MRT-ready).
        std::span<const VkFormat> color_formats{};
        VkFormat                  depth_format = VK_FORMAT_UNDEFINED;
        VkCompareOp               depth_compare_op = VK_COMPARE_OP_GREATER_OR_EQUAL;
        bool                      depth_write = true;  // false for read-only depth (transparent)
        bool                      alpha_blend = false; // SRC_ALPHA / ONE_MINUS_SRC_ALPHA
        VkCullModeFlags           cull_mode  = VK_CULL_MODE_NONE;
        VkFrontFace               front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        // When true the cull mode is a dynamic state (vkCmdSetCullMode, core in
        // Vulkan 1.3) and `cull_mode` is ignored — the pass must set it before
        // drawing (e.g. per-material doubleSided).
        bool                      dynamic_cull_mode = false;

        // Vertex input. Empty (the default) makes a vertex-buffer-less pipeline
        // that draws from SV_VertexID, as the hard-coded triangle does.
        std::span<const VkVertexInputBindingDescription>   vertex_bindings{};
        std::span<const VkVertexInputAttributeDescription> vertex_attributes{};

        // Descriptor set layouts (descriptor-buffer layouts). When non-empty the
        // pipeline is created with the descriptor-buffer bit.
        std::span<const VkDescriptorSetLayout> set_layouts{};

        // Push-constant ranges. When empty, the ranges are derived from shader
        // reflection; pass explicit ranges to override (e.g. a known MVP block).
        std::span<const VkPushConstantRange> push_constants{};
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
