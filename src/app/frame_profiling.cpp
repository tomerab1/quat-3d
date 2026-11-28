// FrameRenderer profiling (ROADMAP R4): the GPU timestamp zones of the render
// graph and the per-frame counters the frame profiler and Tracy plot.

#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app/frame_renderer.hpp"
#include "engine/core/profile.hpp"
#include "engine/scene/instanced_mesh.hpp"

namespace quat::app {

void FrameRenderer::create_gpu_profiler() {
    engine::core::FrameProfiler& profiler = engine::core::profiler();
    auto created = engine::rhi::GpuProfiler::create(
        *device_, engine::rhi::GpuProfiler::Config{.frames_in_flight = k_frames_in_flight});
    if (!created) {
        std::fprintf(stderr, "[warn] GPU profiling disabled: %s\n",
                     created.error().message.c_str());
        profiler.set_gpu_status("disabled: " + created.error().message, false);
        return;
    }
    gpu_profiler_ = std::move(*created);
    profiler.set_gpu_status(std::string(gpu_profiler_.status()), gpu_profiler_.supported());
    if (!gpu_profiler_.supported()) {
        std::fprintf(stderr, "[profile] GPU timing off: %s\n",
                     std::string(gpu_profiler_.status()).c_str());
    }
}

void FrameRenderer::collect_gpu_timings() {
    const std::optional<engine::rhi::GpuFrameResult> frame = gpu_profiler_.collect(frame_);
    if (!frame) return;
    gpu_timings_.clear();
    for (const engine::rhi::GpuZoneResult& z : frame->zones) {
        gpu_timings_.push_back({.id = z.id, .ms = z.ms});
    }
    engine::core::profiler().submit_gpu_frame(frame->frame_index, gpu_timings_, frame->total_ms);
}

void FrameRenderer::plot_frame_counters(const engine::scene::FrameDrawStats& draws) {
    QUAT_PLOT("draws", draws.draws);
    QUAT_PLOT("instances", draws.instances);
    QUAT_PLOT("triangles", draws.triangles);

    // Device-local heaps (VRAM; the unified heap on integrated / Apple GPUs).
    allocator_->heap_stats(heap_stats_);
    double usage = 0.0;
    double budget = 0.0;
    for (const engine::rhi::GpuHeapStats& h : heap_stats_) {
        if (!h.device_local) continue;
        usage += static_cast<double>(h.usage);
        budget += static_cast<double>(h.budget);
    }
    constexpr double k_mib = 1024.0 * 1024.0;
    QUAT_PLOT("vram_usage_mib", usage / k_mib);
    QUAT_PLOT("vram_budget_mib", budget / k_mib);
}

} // namespace quat::app
