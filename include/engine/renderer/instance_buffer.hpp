#pragma once

// InstanceBuffer — host-visible per-instance data for instanced draws (R9.1).
//
// Owned by a pass that lives in one frame slot (the passes are created per
// frame in flight), so rewriting it while recording a frame never races the
// GPU: the slot's previous frame has been waited on. Shaders read it through
// its buffer device address (lib/instancing.slang), so no descriptor is needed.

#include <cstdint>
#include <expected>
#include <span>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/rhi/gpu_allocator.hpp"

namespace engine::rhi {
class Device;
}

namespace engine::renderer {

class InstanceBuffer {
public:
    InstanceBuffer() = default;
    InstanceBuffer(InstanceBuffer&&) noexcept = default;
    InstanceBuffer& operator=(InstanceBuffer&&) noexcept = default;
    InstanceBuffer(const InstanceBuffer&) = delete;
    InstanceBuffer& operator=(const InstanceBuffer&) = delete;

    // Copies `instances` to the start of the buffer (growing it geometrically
    // when it is too small; the old buffer must no longer be in use, which the
    // frame-slot ownership guarantees) and returns its device address. Returns
    // 0 for an empty span.
    [[nodiscard]] std::expected<VkDeviceAddress, core::Error>
    upload(const rhi::Device& device, rhi::GpuAllocator& allocator,
           std::span<const InstanceData> instances);

    // Same for the parallel per-instance motion records (R11); use a separate
    // InstanceBuffer from the one holding the InstanceData.
    [[nodiscard]] std::expected<VkDeviceAddress, core::Error>
    upload(const rhi::Device& device, rhi::GpuAllocator& allocator,
           std::span<const InstanceMotion> motion);

    [[nodiscard]] VkDeviceSize capacity_bytes() const noexcept { return capacity_; }

private:
    [[nodiscard]] std::expected<VkDeviceAddress, core::Error>
    upload_bytes(const rhi::Device& device, rhi::GpuAllocator& allocator, const void* data,
                 VkDeviceSize bytes);

    rhi::GpuBuffer  buffer_;
    VkDeviceAddress address_  = 0;
    VkDeviceSize    capacity_ = 0; // in bytes
};

} // namespace engine::renderer
