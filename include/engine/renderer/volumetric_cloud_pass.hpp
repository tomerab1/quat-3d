#pragma once

// VolumetricCloudPass — the clouds v2 march (ROADMAP R13.4).
//
// Two compute passes per frame at quarter resolution (half the render
// extent in each axis), as Schneider 2015 and Hillaire 2016 render clouds:
//   cloud_march    — each texel marches the cloud model of lib/clouds.slang
//                    along its view ray with a per-frame jittered start
//                    (sun + moon lighting, multiple-scattering octaves,
//                    ambient, cirrus, aerial perspective);
//   cloud_temporal — reprojects last frame's result through the camera
//                    motion and the wind drift, clips it to the current
//                    neighbourhood and blends: a static view converges to
//                    the average of many jittered marches.
// The atmospherics composite upsamples the result over the lit image. The
// history pair is shared by every frame slot (CloudHistory; ping-pong on a
// global frame counter, like TaaHistory).

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

class CloudNoisePass;

inline constexpr VkFormat cloud_color_format = VK_FORMAT_R16G16B16A16_SFLOAT;
// The cloud depth (km) is sampled bilinearly (reprojection, composite):
// R16F filtering is mandatory in Vulkan, R32F filtering is not (Apple GPUs
// expose it only with supports32BitFloatFiltering). Half precision keeps
// ~8 m at 10 km, and CLOUD_NO_DEPTH (1e4 km) is well inside its range.
inline constexpr VkFormat cloud_depth_format = VK_FORMAT_R16_SFLOAT;
// Weight of the current march in the temporal blend (~16 frames of history).
inline constexpr float k_cloud_temporal_blend = 0.06F;

// The quarter-resolution extent for a render extent.
[[nodiscard]] constexpr VkExtent2D cloud_extent(VkExtent2D render) {
    return {(render.width + 1) / 2, (render.height + 1) / 2};
}

struct VolumetricCloudInputs {
    CloudLayerGpu       layer{};
    glm::mat4           inv_view_proj{1.0F};  // this frame, without render jitter
    glm::mat4           prev_view_proj{1.0F}; // last frame, without render jitter
    glm::vec3           camera{0.0F};         // world (m)
    glm::vec2           drift_m{0.0F};        // cloud drift since last frame (world xz, m)
    std::uint64_t       frame_index = 0;
    bool                reset = false;        // camera cut: drop the history
    int                 march_steps = 48;     // samples of a vertical ray (CloudSettings)
    rhi::ResourceHandle depth;                // full-resolution scene depth
    VkExtent2D          render_extent{0, 0};
    rhi::ResourceHandle aerial_perspective;   // AerialPerspectivePass volume
    VkImageView         transmittance = VK_NULL_HANDLE; // AtmospherePass LUTs
    VkImageView         skyview = VK_NULL_HANDLE;
};

// This frame's clouds in the graph (cloud_extent of the render extent).
struct CloudImage {
    rhi::ResourceHandle color; // rgb luminance (sky-view LUT units), a transmittance
    rhi::ResourceHandle depth; // transmittance-weighted cloud distance (km)

    [[nodiscard]] bool valid() const { return color.valid(); }
};

class CloudHistory {
public:
    CloudHistory(const rhi::Device& device, rhi::GpuAllocator& allocator,
                 std::uint32_t frames_in_flight);
    ~CloudHistory();
    CloudHistory(const CloudHistory&) = delete;
    CloudHistory& operator=(const CloudHistory&) = delete;

    struct Slot; // colour + depth image of one ping-pong side (defined in the .cpp)

private:
    friend class VolumetricCloudPass;
    [[nodiscard]] std::expected<void, core::Error> ensure(VkExtent2D extent);

    const rhi::Device*                 device_;    // non-owning
    rhi::GpuAllocator*                 allocator_; // non-owning
    std::uint32_t                      frames_in_flight_;
    VkExtent2D                         extent_{0, 0};
    std::vector<std::unique_ptr<Slot>> slots_;    // two: ping-pong
    std::vector<std::pair<std::uint64_t, std::vector<std::unique_ptr<Slot>>>> retired_;
    std::uint64_t                      frame_ = 0;
    bool                               valid_ = false;
};

class VolumetricCloudPass {
public:
    [[nodiscard]] static std::expected<VolumetricCloudPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, std::shared_ptr<CloudHistory> history);

    VolumetricCloudPass() = default;
    VolumetricCloudPass(VolumetricCloudPass&&) noexcept = default;
    VolumetricCloudPass& operator=(VolumetricCloudPass&&) noexcept = default;
    VolumetricCloudPass(const VolumetricCloudPass&) = delete;
    VolumetricCloudPass& operator=(const VolumetricCloudPass&) = delete;

    [[nodiscard]] bool valid() const { return march_pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares the march and the temporal pass; returns the accumulated
    // clouds. Buffers are rewritten by the next call on this instance (the
    // frame slot's fence must have been waited).
    [[nodiscard]] std::expected<CloudImage, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const VolumetricCloudInputs& in,
                 const CloudNoisePass& noise);

private:
    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning
    std::shared_ptr<CloudHistory> history_;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       march_layout_;
    rhi::ManagedPipeline           march_pipeline_;
    std::vector<std::uint32_t>     march_slots_;
    rhi::DescriptorSetLayout       temporal_layout_;
    rhi::ManagedPipeline           temporal_pipeline_;
    std::vector<std::uint32_t>     temporal_slots_;
    rhi::DescriptorBuffer          march_descriptor_;
    rhi::DescriptorBuffer          temporal_descriptor_;
    rhi::GpuBuffer                 constants_; // CloudLayerGpu | CloudMarchGpu | CloudTemporalGpu
    rhi::Sampler                   clamp_sampler_;
};

} // namespace engine::renderer
