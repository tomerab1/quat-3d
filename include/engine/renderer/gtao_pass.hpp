#pragma once

// GtaoPass — ground-truth ambient occlusion with bent normals (ROADMAP R12).
//
// A port of Intel's XeGTAO (MIT; Jimenez et al. 2016, "Practical Realtime
// Strategies for Accurate Indirect Occlusion"): a full-resolution compute
// main pass (gtao.slang) reads the Hi-Z pyramid's nearest-depth channel at a
// distance-dependent mip (XeGTAO's depth MIP chain) and the GBuffer normal,
// and writes the visibility plus a world-space bent normal; then 0-3
// edge-aware 3x3 denoise passes (gtao_denoise.slang). The noise pattern
// advances every frame (XeGTAO's Hilbert-curve R2 sequence) and TAA does the
// temporal accumulation, as XeGTAO intends.
//
// The output (RGBA8: rgb = bent normal * 0.5 + 0.5, a = visibility) is a
// transient graph image the lighting pass reads (LightingOcclusion::ao):
// bent-normal irradiance, multi-bounce, and cone-based specular occlusion,
// combined with the material's baked occlusion.
//
// Extension point: the horizon search is the shared part of the
// visibility-bitmask variant (SSILVB, Therrien et al. 2023), which replaces
// the two horizon angles with a sector bitmask and adds short-range
// indirect diffuse; it would slot in as another main-pass shader writing the
// same output plus an indirect-light target.

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/hiz_pass.hpp"
#include "engine/renderer/shadow_ao_settings.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

inline constexpr VkFormat gtao_format = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr VkFormat gtao_edges_format = VK_FORMAT_R8_UNORM;

class GtaoPass {
public:
    [[nodiscard]] static std::expected<GtaoPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    GtaoPass() = default;
    GtaoPass(GtaoPass&&) noexcept = default;
    GtaoPass& operator=(GtaoPass&&) noexcept = default;
    GtaoPass(const GtaoPass&) = delete;
    GtaoPass& operator=(const GtaoPass&) = delete;

    // Declares the GTAO main pass and its denoise passes. `view` and
    // `projection` are the camera's (unjittered; perspective only), `normal`
    // the GBuffer's octahedral normal target, `hiz` this frame's pyramid
    // (same extent). `frame_index` advances the noise. Returns an invalid
    // handle when the settings disable GTAO or the projection is not a
    // perspective one.
    [[nodiscard]] std::expected<rhi::ResourceHandle, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const HiZTargets& hiz, rhi::ResourceHandle normal,
                 VkExtent2D extent, const glm::mat4& view, const glm::mat4& projection,
                 const GtaoSettings& settings, std::uint32_t frame_index);

private:
    static constexpr std::uint32_t k_max_denoise = 3;
    struct MainSlots {
        std::uint32_t consts = 0;
        std::uint32_t hiz = 0;
        std::uint32_t normal = 0;
        std::uint32_t point = 0;
        std::uint32_t ao_out = 0;
        std::uint32_t edges_out = 0;
    };
    struct DenoiseSlots {
        std::uint32_t ao_in = 0;
        std::uint32_t edges = 0;
        std::uint32_t ao_out = 0;
    };

    const rhi::Device* device_    = nullptr; // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       main_layout_;
    rhi::ManagedPipeline           main_pipeline_;
    MainSlots                      main_slots_{};
    rhi::DescriptorBuffer          main_descriptor_;
    rhi::DescriptorSetLayout       denoise_layout_;
    rhi::ManagedPipeline           denoise_pipeline_;
    DenoiseSlots                   denoise_slots_{};
    std::array<rhi::DescriptorBuffer, k_max_denoise> denoise_descriptors_;
    rhi::Sampler                   point_sampler_;
    rhi::GpuBuffer                 constants_;
};

} // namespace engine::renderer
