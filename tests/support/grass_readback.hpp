#pragma once

// GPU grass tests: run GrassPass in a render graph and read its blade lists
// and indirect arguments back (through a graph transfer pass, exercising the
// compute -> transfer buffer barriers).
//
//   rhi::RenderGraph graph;                                // or with a pool + GBuffer
//   auto gen = quat::test::run_grass(*g, *pass, graph, frame);
//   gen->blades / gen->args / gen->capacity

#include <cstdint>
#include <cstring>
#include <expected>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/grass_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace quat::test {

// What one generation produced.
struct GrassGenerated {
    engine::renderer::GrassArgsGpu               args{};
    std::vector<engine::renderer::GrassBladeGpu> blades; // near list, then far list (visible only)
    engine::renderer::GrassCapacity              capacity{};
};

// Adds the grass passes (and their draw into `gbuffer` when given) to
// `graph`, then a read-back of the lists, compiles, runs and waits.
[[nodiscard]] inline std::expected<GrassGenerated, engine::core::Error>
run_grass(GpuContext& g, engine::renderer::GrassPass& pass, engine::rhi::RenderGraph& graph,
          const engine::renderer::GrassFrame& frame,
          const engine::renderer::GBufferTargets* gbuffer = nullptr, VkExtent2D extent = {}) {
    namespace r = engine::renderer;
    namespace rhi = engine::rhi;
    auto out = pass.add_to_graph(graph, 0, frame, gbuffer, extent);
    if (!out) return std::unexpected(out.error());
    if (!out->valid()) return GrassGenerated{};
    const r::GrassOutput o = *out;
    const VkDeviceSize blade_bytes =
        static_cast<VkDeviceSize>(o.capacity.near + o.capacity.far) * sizeof(r::GrassBladeGpu);
    const auto host_buffer = [&](VkDeviceSize bytes) {
        return g.allocator.create_buffer(
            bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    auto args = host_buffer(sizeof(r::GrassArgsGpu));
    if (!args) return std::unexpected(args.error());
    auto blades = host_buffer(blade_bytes);
    if (!blades) return std::unexpected(blades.error());
    const VkBuffer args_dst = args->handle();
    const VkBuffer blades_dst = blades->handle();
    (void)graph.add_pass("readback", rhi::PassType::transfer)
        .reads(o.args, rhi::ResourceUsage::transfer_src)
        .reads(o.blades, rhi::ResourceUsage::transfer_src)
        .execute([=](rhi::PassContext& ctx) {
            const VkBufferCopy a{0, 0, sizeof(r::GrassArgsGpu)};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(o.args).buffer, args_dst, 1, &a);
            const VkBufferCopy b{0, 0, blade_bytes};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(o.blades).buffer, blades_dst, 1, &b);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());

    GrassGenerated gen;
    gen.capacity = o.capacity;
    std::memcpy(&gen.args, args->mapped(), sizeof(gen.args));
    const auto* all = static_cast<const r::GrassBladeGpu*>(blades->mapped());
    const std::uint32_t near = gen.args.near.instanceCount;
    const std::uint32_t far = gen.args.far.instanceCount;
    gen.blades.assign(all, all + near);
    // Far blades start at the near region's capacity; the indirect commands
    // keep firstInstance = 0 (drawIndirectFirstInstance is not required) and
    // the draw pass passes the region base as a push constant.
    gen.blades.insert(gen.blades.end(), all + gen.capacity.near, all + gen.capacity.near + far);
    return gen;
}

} // namespace quat::test
