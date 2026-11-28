#pragma once

// AtmosphericsPass — composites the participating media over the lit HDR
// image (ROADMAP R13.3 / R13.4; atmospherics_composite.slang): Hillaire
// 2020's aerial perspective on opaque pixels, the quarter-resolution
// volumetric clouds (VolumetricCloudPass), the analytic height fog beyond
// the froxel grid and the froxel fog (FogPass), in that order from the
// surface toward the camera. One compute pass at the render extent, between
// the lighting pass and the transparents. One instance per frame in flight.

#include <array>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/fog_pass.hpp"
#include "engine/renderer/volumetric_cloud_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

struct AtmosphericsInputs {
    rhi::ResourceHandle hdr;   // lit HDR (render extent), read-modify-written
    rhi::ResourceHandle depth; // scene depth (render extent)
    VkExtent2D          extent{0, 0};
    glm::mat4           inv_view_proj{1.0F}; // jittered, as the HDR was rendered
    glm::vec3           camera{0.0F};
    float               sky_scale = 8.0F;    // sky-view LUT units -> HDR units
    float               pre_exposure = 1.0F;
    rhi::ResourceHandle aerial_perspective;  // invalid: no aerial perspective
    CloudImage          clouds{};            // invalid: no clouds
    rhi::ResourceHandle fog;                 // FogPass volume; invalid: no froxel fog
    FogMedia            media{};             // height fog (analytic beyond the grid)
    bool                height_fog = false;  // apply the analytic height fog
    VkImageView         irradiance = VK_NULL_HANDLE; // IBL diffuse cube (fog ambient)
};

class AtmosphericsPass {
public:
    [[nodiscard]] static std::expected<AtmosphericsPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    AtmosphericsPass();
    ~AtmosphericsPass();
    AtmosphericsPass(AtmosphericsPass&&) noexcept;
    AtmosphericsPass& operator=(AtmosphericsPass&&) noexcept;
    AtmosphericsPass(const AtmosphericsPass&) = delete;
    AtmosphericsPass& operator=(const AtmosphericsPass&) = delete;

    [[nodiscard]] bool valid() const { return pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares the composite. Does nothing (and succeeds) when no input is
    // valid and the height fog is off.
    [[nodiscard]] std::expected<void, core::Error> add_to_graph(rhi::RenderGraph& graph,
                                                                const AtmosphericsInputs& in);

private:
    struct Fallbacks;

    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::vector<std::uint32_t>     slots_;
    rhi::DescriptorBuffer          frame_descriptor_;
    rhi::GpuBuffer                 constants_;
    rhi::Sampler                   sampler_;
    std::unique_ptr<Fallbacks>     fallbacks_;
};

} // namespace engine::renderer
