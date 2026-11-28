#include "engine/rhi/staging_ring.hpp"

#include <cstring>
#include <format>

#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {

std::expected<StagingRing, core::Error> StagingRing::create(GpuAllocator& allocator, VkDeviceSize bytes_per_frame,
                                                            std::uint32_t frames_in_flight) {
    if (bytes_per_frame == 0 || frames_in_flight == 0) {
        return std::unexpected(core::make_error("staging ring: needs a capacity and at least one frame"));
    }
    StagingRing ring;
    ring.capacity_ = bytes_per_frame;
    for (std::uint32_t i = 0; i < frames_in_flight; ++i) {
        auto buffer = allocator.create_staging_buffer(bytes_per_frame);
        if (!buffer) {
            return std::unexpected(core::make_error(std::format("staging ring: {}", buffer.error().message)));
        }
        if (buffer->mapped() == nullptr) {
            return std::unexpected(core::make_error("staging ring: staging buffer is not host-mapped"));
        }
        ring.slots_.push_back(std::move(*buffer));
    }
    return ring;
}

void StagingRing::begin_frame(std::uint64_t frame) {
    slot_ = slots_.empty() ? 0U : static_cast<std::uint32_t>(frame % slots_.size());
    used_ = 0;
    copies_.clear();
}

bool StagingRing::upload(std::span<const std::byte> data, const GpuBuffer& dst, VkDeviceSize dst_offset) {
    if (slots_.empty() || data.empty() || data.size() > remaining() || dst_offset + data.size() > dst.size()) {
        return false;
    }
    const GpuBuffer& slot = slots_[slot_];
    std::memcpy(static_cast<std::byte*>(slot.mapped()) + used_, data.data(), data.size());
    copies_.push_back(Copy{slot.handle(), dst.handle(), VkBufferCopy{used_, dst_offset, data.size()}});
    // Keep copies 16-byte aligned in the slot (vertex data is 4-byte aligned
    // already; alignment helps the copy engine).
    used_ = std::min(capacity_, (used_ + data.size() + 15U) & ~VkDeviceSize{15U});
    return true;
}

void StagingRing::copy(const GpuBuffer& src, VkDeviceSize src_offset, const GpuBuffer& dst, VkDeviceSize dst_offset,
                       VkDeviceSize size) {
    if (size == 0 || src_offset + size > src.size() || dst_offset + size > dst.size()) {
        return;
    }
    copies_.push_back(Copy{src.handle(), dst.handle(), VkBufferCopy{src_offset, dst_offset, size}});
}

std::expected<void, core::Error> StagingRing::add_to_graph(RenderGraph& graph) {
    if (copies_.empty()) {
        return {};
    }
    if (auto flushed = slots_[slot_].flush(0, used_); !flushed) {
        return std::unexpected(flushed.error());
    }
    // Device-to-device copies may read buffers an earlier frame's draws are
    // still reading (vertex fetch): only read-after-read, no barrier needed.
    // Copies into fresh buffers write memory nothing else uses this frame.
    std::vector<Copy> copies = std::move(copies_);
    copies_.clear();
    graph.add_pass("staging_ring_upload", PassType::transfer)
        .execute([copies = std::move(copies)](PassContext& ctx) {
            for (const Copy& c : copies) {
                vkCmdCopyBuffer(ctx.cmd(), c.src, c.dst, 1, &c.region);
            }
            // Later frames draw from (and may copy out of) these buffers.
            VkMemoryBarrier2 barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                                   VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_2_COPY_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(ctx.cmd(), &dep);
        });
    return {};
}

} // namespace engine::rhi
