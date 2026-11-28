#pragma once

// Shared helpers for the GPU particle tests (particle_sim_test.cpp,
// particle_draw_test.cpp): an emitter driven like the app runtime drives it,
// one-frame graph execution with a read-back of the pool, and cameras.

#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <span>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

#include "engine/renderer/particle_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"

namespace quat::test {

namespace pr = engine::renderer;

// An emitter with its CPU emission state, as ParticleRuntime keeps it.
struct TestEmitter {
    pr::ParticleEmitterSettings settings{};
    pr::ParticleSpawnState      spawn{};
    glm::mat4                   world{1.0F};
    std::uint32_t               slot = 0;
    std::uint32_t               base = 0;
    bool                        fresh = true;

    // This frame's input (advances the spawn schedule when dt > 0).
    [[nodiscard]] pr::ParticleEmitterInput input(float dt) {
        pr::ParticleEmitterInput in;
        in.settings = &settings;
        in.info.world = world;
        in.info.slot = slot;
        in.info.pool_base = base;
        in.info.spawn_index = spawn.spawned;
        in.info.spawn_count = dt > 0.0F ? pr::advance_spawn(spawn, settings, dt) : 0;
        in.info.reset = fresh;
        fresh = false;
        return in;
    }
};

struct Camera {
    glm::mat4 view{1.0F};
    glm::mat4 proj{1.0F};
    glm::vec3 eye{0.0F};
};

[[nodiscard]] inline Camera look_at(const glm::vec3& eye, const glm::vec3& target, float aspect = 1.0F) {
    Camera c;
    c.eye = eye;
    c.view = glm::lookAt(eye, target, glm::vec3(0.0F, 1.0F, 0.0F));
    c.proj = glm::perspective(glm::radians(60.0F), aspect, 0.1F, 100.0F);
    c.proj[1][1] *= -1.0F; // Vulkan clip space, as camera_system
    return c;
}

// The pool after one frame.
struct PoolSnapshot {
    std::vector<std::uint32_t>   counters; // 4 per emitter slot
    std::vector<pr::ParticleGpu> particles;
    std::vector<std::uint32_t>   alive;    // two lists of `pool`
    std::vector<std::uint32_t>   draw_args;
    std::uint32_t                pool = 0;

    [[nodiscard]] std::uint32_t dead(std::uint32_t slot) const { return counters[slot * 4]; }
    // The emitter's live alive list (the GPU flips it each simulated frame).
    [[nodiscard]] std::uint32_t live_list(std::uint32_t slot) const { return counters[slot * 4 + 3]; }
    [[nodiscard]] std::uint32_t drawn_count(std::uint32_t slot) const {
        return draw_args[slot * 8 + 1];
    }
    // The emitter's drawn particles, in list (draw) order.
    [[nodiscard]] std::vector<pr::ParticleGpu> drawn_particles(const TestEmitter& e) const {
        std::vector<pr::ParticleGpu> out;
        for (std::uint32_t i = 0; i < drawn_count(e.slot); ++i) {
            out.push_back(particles[alive[live_list(e.slot) * pool + e.base + i]]);
        }
        return out;
    }
    [[nodiscard]] std::vector<std::uint32_t> drawn_slots(const TestEmitter& e) const {
        std::vector<std::uint32_t> out;
        for (std::uint32_t i = 0; i < drawn_count(e.slot); ++i) {
            out.push_back(alive[live_list(e.slot) * pool + e.base + i]);
        }
        return out;
    }
};

// Declares extra inputs (depth, normal, hdr) into the frame's graph.
using TargetsFn = std::function<pr::ParticleTargets(engine::rhi::RenderGraph&)>;

[[nodiscard]] inline std::expected<engine::rhi::GpuBuffer, engine::core::Error>
host_buffer(engine::rhi::GpuAllocator& allocator, VkDeviceSize bytes) {
    return allocator.create_buffer(
        bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

// Runs one frame of `pass` (slot 0) and reads the whole pool back.
[[nodiscard]] inline std::expected<PoolSnapshot, engine::core::Error>
run_particle_frame(GpuContext& g, pr::ParticlePass& pass, const pr::ParticleFrame& frame,
                   const TargetsFn& targets_fn = {}) {
    namespace rhi = engine::rhi;
    rhi::TransientImagePool images(g.device, g.allocator);
    rhi::RenderGraph graph(images);
    const pr::ParticleTargets targets = targets_fn ? targets_fn(graph) : pr::ParticleTargets{};
    auto out = pass.add_to_graph(graph, 0, frame, targets);
    if (!out) return std::unexpected(out.error());
    const std::uint32_t pool = out->pool_capacity;
    const VkDeviceSize sizes[4] = {VkDeviceSize{pr::particle_max_emitters} * 16,
                                   VkDeviceSize{pool} * sizeof(pr::ParticleGpu),
                                   VkDeviceSize{pool} * 8,
                                   VkDeviceSize{pr::particle_max_emitters} * 32};
    std::vector<rhi::GpuBuffer> host;
    for (const VkDeviceSize s : sizes) {
        auto b = host_buffer(g.allocator, s);
        if (!b) return std::unexpected(b.error());
        host.push_back(std::move(*b));
    }
    const rhi::ResourceHandle src[4] = {out->counters, out->particles, out->alive, out->draw_args};
    const VkBuffer dst[4] = {host[0].handle(), host[1].handle(), host[2].handle(), host[3].handle()};
    auto readback = graph.add_pass("particle_readback", rhi::PassType::transfer);
    for (const rhi::ResourceHandle h : src) readback.reads(h, rhi::ResourceUsage::transfer_src);
    readback.execute([=](rhi::PassContext& ctx) {
        for (int i = 0; i < 4; ++i) {
            const VkBufferCopy copy{0, 0, sizes[i]};
            vkCmdCopyBuffer(ctx.cmd(), ctx.resolve(src[i]).buffer, dst[i], 1, &copy);
        }
    });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());
    PoolSnapshot snap;
    snap.pool = pool;
    const auto copy_out = [](rhi::GpuBuffer& b, auto& vec, std::size_t count) {
        (void)b.invalidate_mapped();
        vec.resize(count);
        std::memcpy(vec.data(), b.mapped(), count * sizeof(vec[0]));
    };
    copy_out(host[0], snap.counters, pr::particle_max_emitters * 4);
    copy_out(host[1], snap.particles, pool);
    copy_out(host[2], snap.alive, std::size_t{pool} * 2);
    copy_out(host[3], snap.draw_args, pr::particle_max_emitters * 8);
    return snap;
}

// A frame of `emitters` seen from `camera`.
[[nodiscard]] inline pr::ParticleFrame make_frame(std::span<const pr::ParticleEmitterInput> inputs,
                                                  const Camera& camera, float dt,
                                                  VkExtent2D extent = {64, 64}) {
    pr::ParticleFrame f;
    f.emitters = inputs;
    f.dt = dt;
    f.view = camera.view;
    f.proj = camera.proj;
    f.camera = camera.eye;
    f.extent = extent;
    return f;
}

} // namespace quat::test
