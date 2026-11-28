// InstanceCuller — see include/engine/renderer/instance_cull_pass.hpp.

#include "engine/renderer/instance_cull_pass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#include "compute_kernel.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// instance_cull.slang's resources by name (slots from reflection).
enum Slot : std::uint8_t {
    s_params, s_instances, s_draws, s_chunks, s_cascades, s_submeshes,
    s_counts, s_visible, s_commands, s_draw_counts, s_count
};
constexpr std::array<rhi::BindingRequest, s_count> kResources{{
    {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_instances", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_draws", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_chunks", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_cascades", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_submeshes", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_counts", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_visible", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_commands", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_draw_counts", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

constexpr std::uint32_t kCommandStride = sizeof(VkDrawIndexedIndirectCommand);
static_assert(kCommandStride == 20, "instance_cull.slang writes 5 uints per command");
// Smallest allocation, so small frames never reallocate (and no range is 0).
constexpr VkDeviceSize kMinBytes = 4096;
// Dispatch width before chunks fold into the y dimension.
constexpr std::uint32_t kMaxGroupsX = 65535;

[[nodiscard]] glm::vec4 local_sphere(const asset::Aabb& b) {
    return {(b.min + b.max) * 0.5F, glm::length(b.max - b.min) * 0.5F};
}

} // namespace

std::expected<InstanceCuller, core::Error>
InstanceCuller::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                       rhi::PipelineCache& cache, const std::string& cooked_shader_dir) {
    InstanceCuller out;
    out.device_ = &device;
    out.allocator_ = &allocator;
    out.kernels_ = std::make_unique<Kernels>();
    Kernels& ks = *out.kernels_;
    ks.fns = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!ks.fns.valid()) return fail("instance cull: descriptor buffer functions unavailable");
    const rhi::Device::IndirectCaps caps = device.indirect_caps();
    out.indirect_ = caps.first_instance;
    out.multi_draw_ = caps.multi_draw;
    out.draw_count_ = caps.count && caps.multi_draw;

    for (auto [kernel, entry] : {std::pair{&ks.cull, "compute_main"},
                                 std::pair{&ks.finalize, "finalize_main"}}) {
        auto k = detail::build_compute_kernel(device, ks.fns, cache, cooked_shader_dir,
                                              "instance_cull", kResources, 0, "instance cull",
                                              entry);
        if (!k) return std::unexpected(k.error());
        out.slots_ = k->slots;
        kernel->layout = std::move(k->layout);
        kernel->pipeline = std::move(k->pipeline);
        // Created after the layout reached its final (heap) address.
        auto set = rhi::DescriptorBuffer::create(device, allocator, ks.fns, kernel->layout);
        if (!set) return std::unexpected(set.error());
        kernel->set = std::move(*set);
    }
    if (auto r = out.ensure(out.params_, sizeof(CullParamsGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            true);
        !r) {
        return std::unexpected(r.error());
    }
    if (auto r = out.ensure(out.no_cascades_, sizeof(GpuCascades), 0, true); !r) {
        return std::unexpected(r.error());
    }
    std::memset(out.no_cascades_.mapped(), 0, sizeof(GpuCascades));
    if (auto f = out.no_cascades_.flush(); !f) return std::unexpected(f.error());
    return out;
}

std::expected<void, core::Error> InstanceCuller::ensure(rhi::GpuBuffer& buffer, VkDeviceSize bytes,
                                                        VkBufferUsageFlags usage, bool host) {
    if (buffer.handle() != VK_NULL_HANDLE && buffer.size() >= bytes) return {};
    // Growing frees the old buffer: safe, the frame slot's previous frame is
    // complete (see the header).
    const VkDeviceSize capacity = std::max(kMinBytes, std::bit_ceil(bytes));
    // TRANSFER_SRC: tests and tools read the outputs back.
    const VkBufferUsageFlags all = usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                   (host ? 0U : VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto created = host ? allocator_->create_buffer(
                              capacity, all, VMA_MEMORY_USAGE_AUTO,
                              VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                  VMA_ALLOCATION_CREATE_MAPPED_BIT)
                        : allocator_->create_buffer(capacity, all,
                                                    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);
    if (!created) return std::unexpected(created.error());
    buffer = std::move(*created);
    return {};
}

std::expected<void, core::Error> InstanceCuller::upload_inputs(const InstancedDrawList& list,
                                                               const InstanceCullInput& in) {
    std::vector<CullDrawGpu> draws;
    std::vector<glm::uvec2> chunks;
    std::vector<glm::uvec2> submeshes;
    draws.reserve(list.draws.size());
    records_.clear();
    records_.reserve(list.draws.size());
    for (std::size_t i = 0; i < list.draws.size(); ++i) {
        const InstancedDraw& d = list.draws[i];
        CullDrawGpu g;
        DrawRecord r;
        const bool valid = d.mesh != nullptr && d.mesh->valid() && d.instance_count > 0 &&
                           std::size_t{d.first_instance} + d.instance_count <= list.instances.size();
        if (valid) {
            g.local_sphere = local_sphere(d.mesh->bounds);
            g.first_instance = d.first_instance;
            g.instance_count = d.instance_count;
            g.first_command = static_cast<std::uint32_t>(submeshes.size());
            g.command_count = static_cast<std::uint32_t>(d.mesh->submeshes.size());
            g.shadow_distance = std::max(d.shadow_distance, 0.0F);
            // Skinned vertices leave the bind-pose bounds; a mesh without
            // bounds (an all-zero Aabb, never computed) cannot be culled.
            const asset::Aabb& b = d.mesh->bounds;
            if (d.skinned_vertices != VK_NULL_HANDLE || b.min == b.max) {
                g.flags |= cull_draw_never_cull;
            }
            for (const asset::SubMesh& sm : d.mesh->submeshes) {
                submeshes.emplace_back(sm.index_count, sm.index_offset);
            }
            for (std::uint32_t k = 0; k < d.instance_count; k += cull_chunk_size) {
                chunks.emplace_back(static_cast<std::uint32_t>(i), k);
            }
            r = {g.first_command, g.command_count, g.first_instance, g.instance_count};
        }
        draws.push_back(g);
        records_.push_back(r);
    }
    view_count_ = static_cast<std::uint32_t>(std::min<std::size_t>(in.views.size(), max_cull_views));
    draw_count_total_ = static_cast<std::uint32_t>(draws.size());
    command_count_ = static_cast<std::uint32_t>(submeshes.size());
    instance_count_ = static_cast<std::uint32_t>(list.instances.size());
    chunk_count_ = static_cast<std::uint32_t>(chunks.size());
    groups_x_ = std::min(chunk_count_, kMaxGroupsX);

    const auto put = [this](rhi::GpuBuffer& buffer, const void* data, VkDeviceSize bytes,
                            VkBufferUsageFlags usage) -> std::expected<void, core::Error> {
        if (auto r = ensure(buffer, bytes, usage, true); !r) return r;
        if (bytes == 0) return {};
        std::memcpy(buffer.mapped(), data, bytes);
        return buffer.flush(0, bytes);
    };
    if (auto r = put(draws_, draws.data(), draws.size() * sizeof(CullDrawGpu), 0); !r) return r;
    if (auto r = put(chunks_, chunks.data(), chunks.size() * sizeof(glm::uvec2), 0); !r) return r;
    if (auto r = put(submeshes_, submeshes.data(), submeshes.size() * sizeof(glm::uvec2), 0); !r) {
        return r;
    }

    CullParamsGpu p;
    p.camera = glm::vec4(in.camera, 1.0F);
    p.view_count = view_count_;
    p.chunk_count = chunk_count_;
    p.draw_count = draw_count_total_;
    p.command_count = command_count_;
    p.instance_count = instance_count_;
    p.groups_x = std::max(groups_x_, 1U);
    for (std::uint32_t v = 0; v < view_count_; ++v) {
        // The direct fallback draws every instance: its lists must be full.
        p.views[v] = indirect_ ? in.views[v] : keep_all_cull_view();
    }
    if (auto r = put(params_, &p, sizeof(p), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); !r) return r;

    // GPU-written outputs, one block per view.
    const VkDeviceSize views = std::max(view_count_, 1U);
    const VkDeviceSize per_draw = views * draw_count_total_ * sizeof(std::uint32_t);
    if (auto r = ensure(counts_, per_draw, VK_BUFFER_USAGE_TRANSFER_DST_BIT, false); !r) return r;
    if (auto r = ensure(draw_counts_, per_draw, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false); !r) {
        return r;
    }
    if (auto r = ensure(commands_, views * command_count_ * kCommandStride,
                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, false);
        !r) {
        return r;
    }
    if (auto r = ensure(visible_, views * instance_count_ * sizeof(std::uint32_t), 0, false); !r) {
        return r;
    }
    visible_address_ = rhi::buffer_device_address(device_->handle(), visible_);
    return {};
}

void InstanceCuller::write_sets(const InstanceCullInput& in) {
    const VkDevice vk = device_->handle();
    const auto whole = [vk](const rhi::GpuBuffer& b) {
        return std::pair{rhi::buffer_device_address(vk, b), b.size()};
    };
    const VkDeviceSize instance_bytes =
        std::max<VkDeviceSize>(VkDeviceSize{instance_count_} * sizeof(InstanceData), 16);
    for (Kernel* k : {&kernels_->cull, &kernels_->finalize}) {
        rhi::DescriptorBuffer& set = k->set;
        set.write_uniform_buffer(slots_[s_params], whole(params_).first, sizeof(CullParamsGpu));
        set.write_storage_buffer(slots_[s_instances], in.instances, instance_bytes);
        for (auto [slot, buffer] : {std::pair{s_draws, &draws_}, std::pair{s_chunks, &chunks_},
                                    std::pair{s_submeshes, &submeshes_},
                                    std::pair{s_counts, &counts_}, std::pair{s_visible, &visible_},
                                    std::pair{s_commands, &commands_},
                                    std::pair{s_draw_counts, &draw_counts_}}) {
            const auto [address, size] = whole(*buffer);
            set.write_storage_buffer(slots_[slot], address, size);
        }
        if (in.cascades_address != 0) {
            set.write_storage_buffer(slots_[s_cascades], in.cascades_address, sizeof(GpuCascades));
        } else {
            set.write_storage_buffer(slots_[s_cascades], whole(no_cascades_).first,
                                     sizeof(GpuCascades));
        }
    }
}

std::expected<void, core::Error> InstanceCuller::add_to_graph(rhi::RenderGraph& graph,
                                                              const InstancedDrawList& list,
                                                              const InstanceCullInput& in) {
    if (device_ == nullptr) return fail("instance cull: not created");
    if (in.views.size() > max_cull_views) return fail("instance cull: too many views");
    if (!list.instances.empty() && in.instances == 0) {
        return fail("instance cull: the instance array is not uploaded");
    }
    if (auto r = upload_inputs(list, in); !r) return r;
    h_counts_ = h_visible_ = h_commands_ = h_draw_counts_ = rhi::ResourceHandle{};
    if (view_count_ == 0 || chunk_count_ == 0) return {};
    write_sets(in);

    h_counts_ = graph.import_buffer("cull_counts", counts_.handle(), counts_.size());
    h_visible_ = graph.import_buffer("cull_visible", visible_.handle(), visible_.size());
    h_commands_ = graph.import_buffer("cull_commands", commands_.handle(), commands_.size());
    h_draw_counts_ =
        graph.import_buffer("cull_draw_counts", draw_counts_.handle(), draw_counts_.size());

    const rhi::ResourceHandle counts = h_counts_;
    const VkDeviceSize count_bytes =
        VkDeviceSize{view_count_} * draw_count_total_ * sizeof(std::uint32_t);
    (void)graph.add_pass("instance_cull_reset", rhi::PassType::transfer)
        .writes(counts, rhi::ResourceUsage::transfer_dst)
        .execute([counts, count_bytes](rhi::PassContext& ctx) {
            vkCmdFillBuffer(ctx.cmd(), ctx.resolve(counts).buffer, 0, count_bytes, 0);
        });

    auto cull = graph.add_pass("instance_cull", rhi::PassType::compute);
    if (in.cascades.valid()) cull.reads(in.cascades, rhi::ResourceUsage::buffer_read_compute);
    cull.writes(counts, rhi::ResourceUsage::buffer_write_compute)
        .writes(h_visible_, rhi::ResourceUsage::buffer_write_compute)
        .execute([this](rhi::PassContext& ctx) {
            const std::uint32_t gy = (chunk_count_ + groups_x_ - 1) / groups_x_;
            const VkCommandBuffer cmd = ctx.cmd();
            const Kernel& k = kernels_->cull;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline.handle());
            k.set.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline.layout(), 0);
            vkCmdDispatch(cmd, groups_x_, gy, 1);
        });

    (void)graph.add_pass("instance_cull_finalize", rhi::PassType::compute)
        .reads(counts, rhi::ResourceUsage::buffer_read_compute)
        .writes(h_commands_, rhi::ResourceUsage::buffer_write_compute)
        .writes(h_draw_counts_, rhi::ResourceUsage::buffer_write_compute)
        .execute([this](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const Kernel& k = kernels_->finalize;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline.handle());
            k.set.bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k.pipeline.layout(), 0);
            vkCmdDispatch(cmd, detail::groups_of(view_count_ * draw_count_total_, 64), 1, 1);
        });
    return {};
}

void InstanceCuller::declare_reads(rhi::RenderGraph::PassBuilder& pass) const {
    if (!h_visible_.valid()) return;
    pass.reads(h_visible_, rhi::ResourceUsage::buffer_read_vertex)
        .reads(h_commands_, rhi::ResourceUsage::buffer_read_indirect)
        .reads(h_draw_counts_, rhi::ResourceUsage::buffer_read_indirect);
}

VkDeviceAddress InstanceCuller::visible_address(std::uint32_t view) const {
    if (visible_address_ == 0) return 0;
    return visible_address_ + VkDeviceSize{view} * instance_count_ * sizeof(std::uint32_t);
}

void InstanceCuller::draw(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t draw) const {
    if (draw >= records_.size() || view >= view_count_) return;
    const DrawRecord& r = records_[draw];
    if (r.command_count == 0) return;
    const VkDeviceSize first =
        (VkDeviceSize{view} * command_count_ + r.first_command) * kCommandStride;
    if (!indirect_) {
        // Direct fallback: the visible list holds the draw's whole range.
        const auto* sub = static_cast<const std::uint32_t*>(submeshes_.mapped()) +
                          std::size_t{r.first_command} * 2;
        for (std::uint32_t k = 0; k < r.command_count; ++k) {
            vkCmdDrawIndexed(cmd, sub[2 * k], r.instance_count, sub[2 * k + 1], 0,
                             r.first_instance);
        }
        return;
    }
    if (draw_count_) {
        const VkDeviceSize count_offset =
            (VkDeviceSize{view} * draw_count_total_ + draw) * sizeof(std::uint32_t);
        vkCmdDrawIndexedIndirectCount(cmd, commands_.handle(), first, draw_counts_.handle(),
                                      count_offset, r.command_count, kCommandStride);
    } else if (multi_draw_) {
        vkCmdDrawIndexedIndirect(cmd, commands_.handle(), first, r.command_count, kCommandStride);
    } else {
        for (std::uint32_t k = 0; k < r.command_count; ++k) {
            vkCmdDrawIndexedIndirect(cmd, commands_.handle(), first + VkDeviceSize{k} * kCommandStride,
                                     1, kCommandStride);
        }
    }
}

} // namespace engine::renderer
