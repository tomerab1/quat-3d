#pragma once

// StagingRing — non-blocking uploads into device-local buffers (ROADMAP
// R23.2).
//
// One persistently mapped staging buffer per frame in flight. During a frame,
// callers copy bytes into the current slot (up to its capacity: the per-frame
// upload budget) and queue buffer-to-buffer copies; add_to_graph() records
// them as one transfer pass in that frame's render graph, ending with a
// barrier that makes the writes visible to vertex / index fetch and shader
// reads of every later command on the queue. A slot is reused
// frames_in_flight frames later, after the frame loop has waited for the
// frame that last used it, so nothing here waits on the GPU.
//
// Destination buffers must stay alive until the frame that copied into them
// has completed (keep them, or retire them through a DeferredReleaseQueue).

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {

class RenderGraph;

class StagingRing {
public:
    StagingRing() = default;

    [[nodiscard]] static std::expected<StagingRing, core::Error>
    create(GpuAllocator& allocator, VkDeviceSize bytes_per_frame, std::uint32_t frames_in_flight);

    // Selects the slot of `frame` and clears its copies. Call once per frame
    // before staging; the slot's previous frame must have completed.
    void begin_frame(std::uint64_t frame);

    [[nodiscard]] VkDeviceSize capacity() const { return capacity_; }
    [[nodiscard]] VkDeviceSize remaining() const { return capacity_ - used_; }
    [[nodiscard]] VkDeviceSize staged_bytes() const { return used_; }
    [[nodiscard]] bool valid() const { return !slots_.empty(); }

    // Copies `data` into the slot and queues its copy to dst[dst_offset].
    // False (nothing staged) when it does not fit in remaining().
    [[nodiscard]] bool upload(std::span<const std::byte> data, const GpuBuffer& dst, VkDeviceSize dst_offset);

    // Queues a device-side copy between two buffers (no staging space used).
    void copy(const GpuBuffer& src, VkDeviceSize src_offset, const GpuBuffer& dst, VkDeviceSize dst_offset,
              VkDeviceSize size);

    [[nodiscard]] bool has_work() const { return !copies_.empty(); }

    // Adds this frame's copies to `graph` as one transfer pass (nothing when
    // there are none). The slot's bytes are flushed for non-coherent memory.
    [[nodiscard]] std::expected<void, core::Error> add_to_graph(RenderGraph& graph);

private:
    struct Copy {
        VkBuffer src = VK_NULL_HANDLE;
        VkBuffer dst = VK_NULL_HANDLE;
        VkBufferCopy region{};
    };

    std::vector<GpuBuffer> slots_;
    VkDeviceSize capacity_ = 0;
    VkDeviceSize used_ = 0;
    std::uint32_t slot_ = 0;
    std::vector<Copy> copies_;
};

} // namespace engine::rhi
