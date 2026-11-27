#pragma once

// HiZPass — hierarchical depth pyramid (ROADMAP R12).
//
// Builds an RG32F mip chain from the frame's depth buffer every frame
// (hiz_build.slang, one compute dispatch per level): mip 0 is the depth
// buffer itself, each further texel holds the min (r, nearest) and max
// (g, farthest) depth of its footprint in the level above. It is the shared
// screen-space substrate: GTAO samples its r channel at a distance-dependent
// mip, and later consumers (SSR Hi-Z tracing on r, occlusion culling R6 on g)
// read the same graph resource. The mip-0 dispatch also reduces the min / max
// geometry depth (background excluded) into a small buffer the cascaded
// shadow setup fits its cascades to (SDSM).
//
// The pyramid is a persistent image owned by the pass (one pass per frame
// slot, like every other pass), recreated when the extent changes and
// imported into each frame's graph; nothing in it survives a frame.

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

inline constexpr VkFormat hiz_format = VK_FORMAT_R32G32_SFLOAT;

// The pyramid's graph resources for one frame.
struct HiZTargets {
    // RG32F, mip_count levels; r = min depth, g = max depth. Read it with
    // ResourceUsage::sampled_compute (the view covers every mip).
    rhi::ResourceHandle pyramid;
    // Two uints: the bit patterns of the min and max geometry depth (pixels
    // with depth < 1). min > max when no pixel holds geometry. Read with
    // ResourceUsage::buffer_read_compute.
    rhi::ResourceHandle depth_bounds;
    VkDeviceAddress     depth_bounds_address = 0;
    std::uint32_t       mip_count = 0;
    VkExtent2D          extent{0, 0};

    [[nodiscard]] bool valid() const { return pyramid.valid(); }
};

class HiZPass {
public:
    static constexpr VkDeviceSize k_bounds_bytes = 2 * sizeof(std::uint32_t);

    [[nodiscard]] static std::expected<HiZPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    HiZPass() = default;
    HiZPass(HiZPass&&) noexcept = default;
    HiZPass& operator=(HiZPass&&) noexcept = default;
    HiZPass(const HiZPass&) = delete;
    HiZPass& operator=(const HiZPass&) = delete;

    // Declares the bounds reset and the pyramid build reading `depth`
    // (a D32 depth image of `extent`). Declare after every pass that writes
    // the depth buffer. The returned handles are valid in `graph` only.
    [[nodiscard]] std::expected<HiZTargets, core::Error>
    add_to_graph(rhi::RenderGraph& graph, rhi::ResourceHandle depth, VkExtent2D extent);

    // The pyramid image (tests read its mips back) and the host-visible
    // depth-bounds buffer (valid after the frame's commands completed).
    [[nodiscard]] VkImage image() const { return image_.handle(); }
    [[nodiscard]] const rhi::GpuBuffer& bounds_buffer() const { return bounds_; }

private:
    [[nodiscard]] std::expected<void, core::Error> ensure(VkExtent2D extent);

    // Descriptor slots of hiz_build.slang's resources (from reflection).
    struct Slots {
        std::uint32_t depth = 0;
        std::uint32_t src = 0;
        std::uint32_t dst = 0;
        std::uint32_t bounds = 0;
    };

    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    Slots                          slots_{};

    rhi::GpuBuffer                     bounds_;
    VkDeviceAddress                    bounds_address_ = 0;
    rhi::GpuImage                      image_;
    rhi::ImageView                     full_view_;   // every mip (sampling)
    std::vector<rhi::ImageView>        mip_views_;   // one per mip (storage)
    std::vector<rhi::DescriptorBuffer> descriptors_; // one per dispatch
    std::vector<VkExtent2D>            mip_extents_;
    VkExtent2D                         extent_{0, 0};
};

} // namespace engine::renderer
