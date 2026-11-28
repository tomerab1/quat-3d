#pragma once

// StarFieldPass — bright stars as physical point sources (ROADMAP R22.2.1).
//
// The catalogue (engine::astro::CatalogueStar, the cooked Yale Bright Star
// Catalogue) is uploaded once into a StarCatalogueGpu shared by every frame
// slot. Each frame one compute pass (star_field.slang) rotates the stars
// into the world by SkyState's celestial matrix, attenuates them by the
// atmosphere's transmittance LUT and the cloud layer, applies
// scintillation, bins them into 16x16 screen tiles and adds an
// energy-conserving Gaussian PSF of each into the lit HDR over the sky.
// Radiances are physical (photometry.hpp): a star of illuminance E lux
// covers its pixels with sum(L * Omega_pixel) = E, pre-exposed like every
// light, so daylight hides the stars by exposure alone.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/astro/star_catalogue.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// The catalogue on the GPU (immutable; share it between frame slots).
class StarCatalogueGpu {
    struct Key {
        explicit Key() = default;
    };

public:
    [[nodiscard]] static std::expected<std::shared_ptr<const StarCatalogueGpu>, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator,
           std::span<const astro::CatalogueStar> stars);

    explicit StarCatalogueGpu(Key /*key*/) {}

    [[nodiscard]] VkDeviceAddress address() const { return address_; }
    [[nodiscard]] VkDeviceSize    size() const { return size_; }
    [[nodiscard]] std::uint32_t   count() const { return count_; }

private:
    rhi::GpuBuffer  buffer_;
    VkDeviceAddress address_ = 0;
    VkDeviceSize    size_ = 0;
    std::uint32_t   count_ = 0;
};

// One frame's view of the sky.
struct StarFieldFrame {
    glm::mat4 view_proj{1.0F};  // world -> clip, the frame's jittered raster matrix
    glm::mat4 projection{1.0F}; // the camera projection (pixel solid angle)
    glm::vec3 camera_forward{0.0F, 0.0F, -1.0F}; // world
    glm::mat3 world_from_celestial{1.0F};        // astro::SkyState
    VkExtent2D extent{0, 0};    // the HDR target's (render) extent
    float pre_exposure = 1.0F;  // photometry.hpp
    float time_s = 0.0F;        // scintillation clock (simulated time)
    float scintillation = 1.0F; // strength (0 = steady)
    glm::vec3     sun_direction{0.0F, 1.0F, 0.0F}; // toward the sun (cloud march)
    CloudSettings clouds{};     // the sky's cloud layer (disabled: clear)
};

class StarFieldPass {
public:
    // Screen tile edge (pixels) and the most stars one tile lists.
    static constexpr std::uint32_t k_tile = 16;
    static constexpr std::uint32_t k_tile_capacity = 32;

    [[nodiscard]] static std::expected<StarFieldPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir,
           std::shared_ptr<const StarCatalogueGpu> catalogue);

    StarFieldPass() = default;
    StarFieldPass(StarFieldPass&&) noexcept = default;
    StarFieldPass& operator=(StarFieldPass&&) noexcept = default;
    StarFieldPass(const StarFieldPass&) = delete;
    StarFieldPass& operator=(const StarFieldPass&) = delete;

    [[nodiscard]] bool valid() const { return catalogue_ != nullptr; }

    // Adds the stars to `hdr` (read-modify-write) where `depth` is the far
    // plane (1). `transmittance` is AtmospherePass::transmittance_view()
    // (SHADER_READ_ONLY_OPTIMAL). Buffers belong to this pass and hold this
    // call's values until the next call (one call per graph execution).
    [[nodiscard]] std::expected<void, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle hdr, rhi::ResourceHandle depth,
                 VkImageView transmittance, const StarFieldFrame& frame);

private:
    [[nodiscard]] std::expected<void, core::Error> ensure_tiles(VkDeviceSize tiles);

    const rhi::Device* device_ = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;
    std::shared_ptr<const StarCatalogueGpu> catalogue_;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           splat_pipeline_;
    rhi::ManagedPipeline           resolve_pipeline_;
    rhi::Sampler                   sampler_;

    rhi::GpuBuffer constants_; // StarFrameGpu, host-visible
    rhi::GpuBuffer visible_;   // one VisibleStar per catalogue star
    rhi::GpuBuffer tile_counts_;
    rhi::GpuBuffer tile_lists_;
    VkDeviceSize   tile_slots_ = 0; // tiles the tile buffers hold

    rhi::DescriptorBuffer descriptor_; // rebuilt each add_to_graph call
};

} // namespace engine::renderer
