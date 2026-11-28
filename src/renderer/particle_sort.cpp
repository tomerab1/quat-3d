// ParticlePass's optional back-to-front sort (particle_sort.slang): a bitonic
// network over each sorted emitter's power-of-two key range — stages up to
// the 512-key block in shared memory, longer compare distances in device
// memory, compute barriers between the dispatches.

#include <cstdint>
#include <utility>
#include <vector>

#include "particle_internal.hpp"

namespace engine::renderer {

namespace {

constexpr std::uint32_t kSortGroup = 256; // particle_sort.slang numthreads
constexpr std::uint32_t kSortBlock = 512; // particle_sort.slang BLOCK

// Orders one sort dispatch's key writes before the next one's accesses.
void compute_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace

void declare_particle_sort(ParticlePass::State& s, ParticleSlot& slot, rhi::RenderGraph& graph,
                           const ParticleGraphOutput& buffers, rhi::ResourceHandle keys,
                           std::vector<std::pair<std::uint32_t, std::uint32_t>> jobs) {
    using U = rhi::ResourceUsage;
    (void)graph.add_pass("particle_sort", rhi::PassType::compute)
        .reads(buffers.particles, U::buffer_read_compute)
        .reads(buffers.counters, U::buffer_read_compute)
        .writes(buffers.alive, U::buffer_write_compute)
        .writes(keys, U::buffer_write_compute)
        .execute([&s, set = &slot.sort_set, jobs = std::move(jobs)](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            for (const auto& [e, n] : jobs) {
                particle_dispatch(cmd, s.sort_keys, *set, {e, 0, 0, 0}, n / kSortGroup);
                compute_barrier(cmd);
                particle_dispatch(cmd, s.sort_local, *set, {e, 0, 0, 0}, n / kSortBlock);
                compute_barrier(cmd);
                for (std::uint32_t k = 2 * kSortBlock; k <= n; k <<= 1) {
                    for (std::uint32_t j = k / 2; j >= kSortBlock; j >>= 1) {
                        particle_dispatch(cmd, s.sort_global, *set, {e, k, j, 0},
                                          n / 2 / kSortGroup);
                        compute_barrier(cmd);
                    }
                    particle_dispatch(cmd, s.sort_merge, *set, {e, k, 0, 0}, n / kSortBlock);
                    compute_barrier(cmd);
                }
                particle_dispatch(cmd, s.sort_scatter, *set, {e, 0, 0, 0}, n / kSortGroup);
            }
        });
}

} // namespace engine::renderer
