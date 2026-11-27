#include "engine/renderer/light_cull_pass.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kComputeStage = VK_SHADER_STAGE_COMPUTE_BIT;

// light_cull.slang's numthreads.
constexpr std::uint32_t kCullGroupSize = 64;

constexpr VkDeviceSize kParamsBytes = sizeof(ClusterParams);
constexpr VkDeviceSize kLightsBytes = static_cast<VkDeviceSize>(max_lights) * sizeof(GpuLight);
constexpr VkDeviceSize kGridBytes =
    static_cast<VkDeviceSize>(cluster_count) * 2 * sizeof(std::uint32_t);
constexpr VkDeviceSize kIndicesBytes =
    static_cast<VkDeviceSize>(max_cluster_light_indices) * sizeof(std::uint32_t);
constexpr VkDeviceSize kCounterBytes = sizeof(std::uint32_t);

// light_cull.slang set 0: params, lights, grid, indices, counter.
[[nodiscard]] std::array<rhi::DescriptorBinding, 5> cull_bindings() {
    return {{
        {0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, kComputeStage},
        {0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
        {0, 4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kComputeStage},
    }};
}

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
host_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(
        size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

[[nodiscard]] std::expected<rhi::GpuBuffer, core::Error>
device_buffer(rhi::GpuAllocator& allocator, VkDeviceSize size, VkBufferUsageFlags usage) {
    return allocator.create_buffer(size, usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                   VMA_MEMORY_USAGE_AUTO);
}

} // namespace

std::array<rhi::DescriptorBinding, 4> clustered_light_bindings(std::uint32_t first_binding,
                                                               VkShaderStageFlags stages) {
    return {{
        {0, first_binding + 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages},
        {0, first_binding + 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages},
        {0, first_binding + 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages},
        {0, first_binding + 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages},
    }};
}

void write_clustered_light_descriptors(rhi::DescriptorBuffer& descriptor,
                                       std::uint32_t first_binding,
                                       const ClusteredLightBuffers& buffers) {
    descriptor.write_uniform_buffer(first_binding + 0, buffers.params, buffers.params_size);
    descriptor.write_storage_buffer(first_binding + 1, buffers.lights, buffers.lights_size);
    descriptor.write_storage_buffer(first_binding + 2, buffers.grid, buffers.grid_size);
    descriptor.write_storage_buffer(first_binding + 3, buffers.indices, buffers.indices_size);
}

void read_clustered_lights(rhi::RenderGraph::PassBuilder& pass, const ClusteredLights& lights,
                           rhi::ResourceUsage usage) {
    if (!lights.valid()) return;
    pass.reads(lights.params, usage)
        .reads(lights.lights, usage)
        .reads(lights.grid, usage)
        .reads(lights.indices, usage);
}

// ---------------------------------------------------------------------------
// EmptyLightClusters
// ---------------------------------------------------------------------------
std::expected<EmptyLightClusters, core::Error>
EmptyLightClusters::create(const rhi::Device& device, rhi::GpuAllocator& allocator) {
    EmptyLightClusters out;
    auto params = host_buffer(allocator, kParamsBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!params) return std::unexpected(params.error());
    // Big enough for one GpuLight / grid entry / index; never read (no lights).
    auto storage = host_buffer(allocator, sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!storage) return std::unexpected(storage.error());

    ClusterParams empty{};
    empty.dims.w = 0; // no lights: consumers skip the cluster lookup entirely
    std::memcpy(params->mapped(), &empty, sizeof(empty));
    params->flush();
    std::memset(storage->mapped(), 0, sizeof(GpuLight));
    storage->flush();

    out.params_ = std::move(*params);
    out.storage_ = std::move(*storage);
    const VkDeviceAddress storage_address =
        rhi::buffer_device_address(device.handle(), out.storage_);
    out.buffers_.params = rhi::buffer_device_address(device.handle(), out.params_);
    out.buffers_.params_size = kParamsBytes;
    out.buffers_.lights = storage_address;
    out.buffers_.grid = storage_address;
    out.buffers_.indices = storage_address;
    out.buffers_.lights_size = sizeof(GpuLight);
    out.buffers_.grid_size = sizeof(GpuLight);
    out.buffers_.indices_size = sizeof(GpuLight);
    return out;
}

// ---------------------------------------------------------------------------
// LightCullPass
// ---------------------------------------------------------------------------
std::expected<LightCullPass, core::Error>
LightCullPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                      rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    LightCullPass out;
    out.state_ = std::make_unique<State>();
    State& s = *out.state_;

    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("light cull pass: descriptor buffer functions unavailable");

    const auto bindings = cull_bindings();
    auto layout = rhi::DescriptorSetLayout::create(device, s.fns, bindings);
    if (!layout) return std::unexpected(layout.error());
    s.layout = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/light_cull.spv");
    if (!shader) return std::unexpected(shader.error());
    const VkDescriptorSetLayout set_layout = s.layout.handle();
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    auto pipeline = rhi::ComputePipeline::create(device, cache.handle(), info);
    if (!pipeline) return std::unexpected(pipeline.error());
    s.pipeline = std::move(*pipeline);

    auto params = host_buffer(allocator, kParamsBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (!params) return std::unexpected(params.error());
    s.params = std::move(*params);
    auto lights = host_buffer(allocator, kLightsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    if (!lights) return std::unexpected(lights.error());
    s.lights = std::move(*lights);
    // The grid / index list / cursor are GPU-written; TRANSFER_SRC lets tests
    // and tools read them back.
    auto grid = device_buffer(allocator, kGridBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!grid) return std::unexpected(grid.error());
    s.grid = std::move(*grid);
    auto indices = device_buffer(allocator, kIndicesBytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!indices) return std::unexpected(indices.error());
    s.indices = std::move(*indices);
    auto counter = device_buffer(allocator, kCounterBytes,
                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!counter) return std::unexpected(counter.error());
    s.counter = std::move(*counter);

    const VkDevice vk = device.handle();
    s.addresses.params = rhi::buffer_device_address(vk, s.params);
    s.addresses.lights = rhi::buffer_device_address(vk, s.lights);
    s.addresses.grid = rhi::buffer_device_address(vk, s.grid);
    s.addresses.indices = rhi::buffer_device_address(vk, s.indices);
    s.addresses.params_size = kParamsBytes;
    s.addresses.lights_size = kLightsBytes;
    s.addresses.grid_size = kGridBytes;
    s.addresses.indices_size = kIndicesBytes;

    // The buffers never move, so the cull descriptors are written once.
    auto descriptor = rhi::DescriptorBuffer::create(device, allocator, s.fns, s.layout);
    if (!descriptor) return std::unexpected(descriptor.error());
    s.descriptor = std::move(*descriptor);
    s.descriptor.write_uniform_buffer(0, s.addresses.params, kParamsBytes);
    s.descriptor.write_storage_buffer(1, s.addresses.lights, kLightsBytes);
    s.descriptor.write_storage_buffer(2, s.addresses.grid, kGridBytes);
    s.descriptor.write_storage_buffer(3, s.addresses.indices, kIndicesBytes);
    s.descriptor.write_storage_buffer(4, rhi::buffer_device_address(vk, s.counter),
                                      kCounterBytes);
    return out;
}

std::expected<ClusteredLights, core::Error>
LightCullPass::add_to_graph(rhi::RenderGraph& graph, const glm::mat4& view, const glm::mat4& proj,
                            std::span<const GpuLight> lights) {
    if (!state_) return fail("light cull pass: not created");
    State& s = *state_;

    const auto count = static_cast<std::uint32_t>(
        std::min<std::size_t>(lights.size(), static_cast<std::size_t>(max_lights)));
    auto params = make_cluster_params(view, proj, count);
    if (!params) return std::unexpected(params.error());

    // Host writes land before vkQueueSubmit, which makes them visible to the
    // device (flushed for non-coherent memory) — no graph barrier needed.
    std::memcpy(s.params.mapped(), &*params, sizeof(ClusterParams));
    s.params.flush(0, kParamsBytes);
    if (count > 0) {
        const std::size_t bytes = static_cast<std::size_t>(count) * sizeof(GpuLight);
        std::memcpy(s.lights.mapped(), lights.data(), bytes);
        s.lights.flush(0, bytes);
    }

    ClusteredLights out;
    out.buffers = s.addresses;
    out.light_count = count;
    out.params = graph.import_buffer("cluster_params", s.params.handle(), kParamsBytes);
    out.lights = graph.import_buffer("cluster_lights", s.lights.handle(), kLightsBytes);
    out.grid = graph.import_buffer("cluster_grid", s.grid.handle(), kGridBytes);
    out.indices = graph.import_buffer("cluster_indices", s.indices.handle(), kIndicesBytes);
    out.counter = graph.import_buffer("cluster_counter", s.counter.handle(), kCounterBytes);

    const rhi::ResourceHandle counter = out.counter;
    (void)graph.add_pass("light_cull_reset", rhi::PassType::transfer)
        .writes(counter, rhi::ResourceUsage::transfer_dst)
        .execute([counter](rhi::PassContext& ctx) {
            vkCmdFillBuffer(ctx.cmd(), ctx.resolve(counter).buffer, 0, kCounterBytes, 0);
        });

    (void)graph.add_pass("light_cull", rhi::PassType::compute)
        .reads(out.params, rhi::ResourceUsage::buffer_read_compute)
        .reads(out.lights, rhi::ResourceUsage::buffer_read_compute)
        .writes(out.counter, rhi::ResourceUsage::buffer_write_compute)
        .writes(out.grid, rhi::ResourceUsage::buffer_write_compute)
        .writes(out.indices, rhi::ResourceUsage::buffer_write_compute)
        .execute([state = &s](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline.handle());
            state->descriptor.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, state->pipeline.layout(),
                                   0);
            vkCmdDispatch(cmd, (cluster_count + kCullGroupSize - 1) / kCullGroupSize, 1, 1);
        });
    return out;
}

} // namespace engine::renderer
