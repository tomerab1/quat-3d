#pragma once

// DynamicIbl — incrementally refreshed environment probe (Phase 11, Slice 11.3).
//
// Replaces blocking IblBaker rebakes for the live scene: the probe re-renders
// one small compute step per frame inside the render graph (env-cube faces in
// 128x128 tiles to bound the per-frame cloud-march cost, the irradiance
// convolution, then one prefiltered-specular mip per frame — 30 steps per
// cycle), into the back set of two ping-ponged map sets. The front set is what
// lighting samples; it flips when a cycle completes, so ambient/reflections
// track the moving sun and drifting clouds with zero pipeline stalls. Between
// cycles the probe idles unless its inputs changed or the cloud-drift cadence
// is due. The BRDF LUT is sun-independent and baked once.
//
// Synchronisation: each step's pass carries its own sync2 barriers (the maps
// are multi-mip/layer persistent images, outside the graph's resource
// tracking). Declare the step pass BEFORE any pass that samples views() — the
// graph keeps declaration order for independent passes — so the post-flip
// frame's reads land after the cycle's trailing release barrier.

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

// Inputs of one probe refresh. Captured at cycle start so all six env faces
// see consistent sun/cloud parameters; when skyview/transmittance are both set
// the env bakes from the atmosphere LUTs, otherwise the procedural sky.
struct IblCycleParams {
    glm::vec3     sun_to{0.0F, 1.0F, 0.0F}; // unit, toward the sun
    VkImageView   skyview = VK_NULL_HANDLE;
    VkImageView   transmittance = VK_NULL_HANDLE;
    CloudSettings clouds{};
};

class DynamicIbl {
public:
    // Builds pipelines + both map sets, bakes the BRDF LUT, and runs one full
    // blocking cycle with `initial` so views() is complete from frame one.
    [[nodiscard]] static std::expected<DynamicIbl, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir, const IblCycleParams& initial);

    DynamicIbl() = default;
    DynamicIbl(DynamicIbl&&) noexcept = default;
    DynamicIbl& operator=(DynamicIbl&&) noexcept = default;
    DynamicIbl(const DynamicIbl&) = delete;
    DynamicIbl& operator=(const DynamicIbl&) = delete;

    [[nodiscard]] bool valid() const { return device_ != nullptr; }

    // Views of the most recently completed probe, for lighting/transparent.
    // The handles change on cycle completion — fetch every frame, after
    // add_step_to_graph.
    [[nodiscard]] IblViewSet views() const;

    // Record the next refresh step into the frame graph (call once per frame,
    // before declaring any pass that samples views()). `params` is latched at
    // the start of each cycle. Between cycles the probe idles unless the sun
    // or cloud settings changed, or the cloud drift cadence is due — idle
    // frames record nothing and cost nothing.
    [[nodiscard]] std::expected<void, core::Error>
    add_step_to_graph(rhi::RenderGraph& graph, const IblCycleParams& params);

    // Raw front-set image for the self-test's readback.
    [[nodiscard]] VkImage front_irradiance_image() const {
        return sets_[front_].irradiance.handle();
    }

private:
    struct ProbeSet {
        rhi::GpuImage  irradiance;  // 16x16x6
        rhi::GpuImage  prefiltered; // 128x128x6, prefilter_mip_count mips
        rhi::ImageView irr_cube;    // sampled
        rhi::ImageView pre_cube;    // sampled, all mips
        rhi::ImageView irr_store;   // 2D array
        std::vector<rhi::ImageView> pre_store; // 2D array per mip
    };

    // One descriptor set per step; fallible, so done outside the execute lambda.
    [[nodiscard]] std::expected<rhi::DescriptorBuffer, core::Error>
    make_step_db(std::uint32_t step, std::uint32_t back, const IblCycleParams& params);
    // Barriers + bind + dispatch for `step`, writing probe set `back`. The
    // back index is latched at record time — front_ flips before the cycle's
    // last lambda executes, so it must not be re-read here.
    void record_step(VkCommandBuffer cmd, std::uint32_t step, std::uint32_t back,
                     const IblCycleParams& params, rhi::DescriptorBuffer& db);

    const rhi::Device* device_    = nullptr;
    rhi::GpuAllocator* allocator_ = nullptr;

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       env_layout_;
    rhi::DescriptorSetLayout       conv_layout_;
    rhi::ComputePipeline           env_pipeline_;
    rhi::ComputePipeline           irradiance_pipeline_;
    rhi::ComputePipeline           prefilter_pipeline_;
    rhi::ComputePipeline           lut_pipeline_;
    rhi::Sampler                   sampler_;

    rhi::GpuImage  env_;       // shared scratch env cube (GENERAL for life)
    rhi::ImageView env_cube_;  // sampled by the convolutions
    rhi::ImageView env_store_; // 2D array storage
    rhi::GpuImage  brdf_lut_;  // static, baked once
    rhi::ImageView brdf_view_;
    rhi::GpuImage  fallback_lut_; // 1x1 black for unused LUT slots
    rhi::ImageView fallback_view_;

    std::array<ProbeSet, 2> sets_;
    std::uint32_t           front_ = 0;
    std::uint32_t           step_  = 0; // next step of the current cycle
    IblCycleParams          cycle_params_{};
    float                   last_cycle_start_s_ = 0.0F; // clouds.time_s clock

    // Per-frame step descriptors, ring-buffered past frames-in-flight (2).
    std::array<rhi::DescriptorBuffer, 3> db_ring_;
    std::size_t                          ring_ = 0;
};

// Self-test: a full blocking cycle (procedural sky) yields sky-blue zenith
// irradiance; a second cycle with a low sun flips the sets and lands a
// measurably different irradiance in the new front set.
[[nodiscard]] std::expected<void, core::Error>
run_dynamic_ibl_self_test(const rhi::Device& device, rhi::GpuAllocator& allocator,
                          rhi::PipelineCache& cache, const std::string& cooked_shader_dir);

} // namespace engine::renderer
