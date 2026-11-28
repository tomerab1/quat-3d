#include "engine/renderer/terrain_edit_pass.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "engine/rhi/device.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

constexpr VkShaderStageFlags kCompute = VK_SHADER_STAGE_COMPUTE_BIT;
constexpr std::uint32_t kGroup = 8; // numthreads(8, 8, 1)

// Mirrors Push in terrain_edit.slang (28 bytes).
struct EditPush {
    std::int32_t  tile_x = 0;
    std::int32_t  tile_z = 0;
    std::uint32_t res = 0;
    float         texel_m = 0.0F;
    float         half_size = 0.0F;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};
static_assert(sizeof(EditPush) == 28, "must match terrain_edit.slang Push");

enum Kernel : std::uint8_t { k_init, k_pointwise, k_copy, k_smooth, k_kernel_count };
constexpr std::array<const char*, k_kernel_count> kEntries{"edit_init", "edit_pointwise",
                                                           "edit_copy", "edit_smooth"};

enum Resource : std::uint8_t {
    r_stamps, r_points, r_edges, r_heights, r_base, r_scratch, r_paint, r_count
};
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_stamps", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_points", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_edges", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_heights", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_base", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_scratch", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_paint", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

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

struct TerrainEditPass::State {
    const rhi::Device*                              device = nullptr;
    rhi::GpuAllocator*                              allocator = nullptr;
    rhi::DescriptorBufferFunctions                  fns{};
    rhi::DescriptorSetLayout                        layout;
    std::array<std::uint32_t, r_count>              slots{};
    std::array<rhi::ManagedPipeline, k_kernel_count> pipelines;
};

TerrainEditPass::TerrainEditPass() = default;
TerrainEditPass::~TerrainEditPass() = default;
TerrainEditPass::TerrainEditPass(TerrainEditPass&&) noexcept = default;
TerrainEditPass& TerrainEditPass::operator=(TerrainEditPass&&) noexcept = default;

std::expected<TerrainEditPass, core::Error>
TerrainEditPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                        rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    TerrainEditPass out;
    out.state_ = std::make_unique<State>();
    State& s = *out.state_;
    s.device = &device;
    s.allocator = &allocator;
    s.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!s.fns.valid()) return fail("terrain edit: descriptor buffer functions unavailable");

    auto shader = cache.load(cooked_shader_dir + "/terrain_edit.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kResources, kCompute);
    if (!bindings) return fail("terrain edit: " + bindings.error().message);
    for (std::size_t r = 0; r < r_count; ++r) s.slots[r] = (*bindings)[r].binding;
    auto layout = rhi::DescriptorSetLayout::create(device, s.fns, *bindings);
    if (!layout) return std::unexpected(layout.error());
    s.layout = std::move(*layout);

    const VkDescriptorSetLayout set_layout = s.layout.handle();
    const VkPushConstantRange push{kCompute, 0, sizeof(EditPush)};
    for (std::size_t k = 0; k < k_kernel_count; ++k) {
        rhi::ComputePipeline::CreateInfo info{};
        info.shader = *shader;
        info.entry = kEntries[k];
        info.set_layouts = {&set_layout, 1};
        info.push_constants = {&push, 1};
        auto pipeline = cache.create_compute_pipeline(info);
        if (!pipeline) return std::unexpected(pipeline.error());
        s.pipelines[k] = std::move(*pipeline);
    }
    return out;
}

std::expected<TerrainEditPass::Target, core::Error>
TerrainEditPass::prepare(terrain::PackedStamps packed, glm::ivec2 tile, std::uint32_t res,
                         float tile_size_m, VkDeviceAddress heights,
                         VkDeviceSize heights_bytes) const {
    if (!state_) return fail("terrain edit: not created");
    const State& s = *state_;
    const VkDevice vk = s.device->handle();
    Target t;
    t.tile = tile;
    t.res = res;
    t.tile_size_m = tile_size_m;
    // Empty arrays still get one element (a zero-sized buffer is invalid).
    if (packed.points.empty()) packed.points.emplace_back(0.0F);
    if (packed.edges.empty()) packed.edges.emplace_back(0.0F);
    t.packed = std::move(packed);

    const auto upload = [&](rhi::GpuBuffer& out, const void* data,
                            VkDeviceSize bytes) -> std::expected<void, core::Error> {
        auto b = s.allocator->create_buffer(
            std::max<VkDeviceSize>(bytes, 16),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (!b) return std::unexpected(b.error());
        std::memcpy(b->mapped(), data, bytes);
        if (auto f = b->flush(); !f) return std::unexpected(f.error());
        out = std::move(*b);
        return {};
    };
    const auto local = [&](rhi::GpuBuffer& out, VkDeviceSize bytes,
                           VkBufferUsageFlags extra) -> std::expected<void, core::Error> {
        auto b = s.allocator->create_buffer(bytes,
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | extra,
                                            VMA_MEMORY_USAGE_AUTO);
        if (!b) return std::unexpected(b.error());
        out = std::move(*b);
        return {};
    };
    const std::span<const terrain::PackedStamp> stamps = t.packed.stamps;
    if (auto r = upload(t.stamps, stamps.data(), stamps.size_bytes()); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = upload(t.points, t.packed.points.data(),
                        t.packed.points.size() * sizeof(glm::vec2));
        !r) {
        return std::unexpected(r.error());
    }
    if (auto r = upload(t.edges, t.packed.edges.data(), t.packed.edges.size() * sizeof(glm::vec4));
        !r) {
        return std::unexpected(r.error());
    }
    const VkDeviceSize n = static_cast<VkDeviceSize>(res) * res;
    if (auto r = local(t.base, n * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT); !r) {
        return std::unexpected(r.error());
    }
    if (auto r = local(t.scratch, n * sizeof(float), 0); !r) return std::unexpected(r.error());
    if (auto r = local(t.paint, n * sizeof(std::uint32_t), VK_BUFFER_USAGE_TRANSFER_SRC_BIT); !r) {
        return std::unexpected(r.error());
    }

    auto descriptor = rhi::DescriptorBuffer::create(*s.device, *s.allocator, s.fns, s.layout);
    if (!descriptor) return std::unexpected(descriptor.error());
    t.descriptor = std::move(*descriptor);
    const auto storage = [&](Resource r, const rhi::GpuBuffer& buffer) {
        t.descriptor.write_storage_buffer(s.slots[r], rhi::buffer_device_address(vk, buffer),
                                          buffer.size());
    };
    storage(r_stamps, t.stamps);
    storage(r_points, t.points);
    storage(r_edges, t.edges);
    t.descriptor.write_storage_buffer(s.slots[r_heights], heights, heights_bytes);
    storage(r_base, t.base);
    storage(r_scratch, t.scratch);
    storage(r_paint, t.paint);
    return t;
}

void TerrainEditPass::record(VkCommandBuffer cmd, const Target& t) const {
    if (!state_) return;
    const State& s = *state_;
    EditPush push;
    push.tile_x = t.tile.x;
    push.tile_z = t.tile.y;
    push.res = t.res;
    // TileFrame::texel_m / texel_position's constants, computed here once.
    push.texel_m = t.tile_size_m / static_cast<float>(t.res - 1);
    push.half_size = 0.5F * t.tile_size_m;
    const std::uint32_t groups = (t.res + kGroup - 1) / kGroup;
    bool bound = false;
    bool first_dispatch = true;
    const auto dispatch = [&](Kernel kernel, std::uint32_t first, std::uint32_t count) {
        if (!first_dispatch) compute_barrier(cmd);
        first_dispatch = false;
        const rhi::ManagedPipeline& pipeline = s.pipelines[kernel];
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.handle());
        if (!bound) {
            t.descriptor.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout(), 0);
            bound = true;
        }
        push.first = first;
        push.count = count;
        vkCmdPushConstants(cmd, pipeline.layout(), kCompute, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, groups, groups, 1);
    };

    dispatch(k_init, 0, 0);
    const auto& stamps = t.packed.stamps;
    const auto smooth = static_cast<std::uint32_t>(terrain::StampOp::smooth);
    for (std::uint32_t i = 0; i < stamps.size();) {
        if (stamps[i].op != smooth) {
            std::uint32_t end = i;
            while (end < stamps.size() && stamps[end].op != smooth) ++end;
            dispatch(k_pointwise, i, end - i);
            i = end;
            continue;
        }
        for (std::uint32_t pass = 0; pass < stamps[i].iterations; ++pass) {
            dispatch(k_copy, 0, 0);
            dispatch(k_smooth, i, 1);
        }
        ++i;
    }
}

} // namespace engine::renderer
