#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::rhi {

class Device;

// VK_EXT_descriptor_buffer device entry points (not exported by the loader).
struct DescriptorBufferFunctions {
    PFN_vkGetDescriptorSetLayoutSizeEXT          get_layout_size = nullptr;
    PFN_vkGetDescriptorSetLayoutBindingOffsetEXT get_binding_offset = nullptr;
    PFN_vkGetDescriptorEXT                       get_descriptor = nullptr;
    PFN_vkCmdBindDescriptorBuffersEXT            cmd_bind_buffers = nullptr;
    PFN_vkCmdSetDescriptorBufferOffsetsEXT       cmd_set_offsets = nullptr;

    [[nodiscard]] static DescriptorBufferFunctions load(VkDevice device);
    [[nodiscard]] bool valid() const;
};

// A VkDescriptorSetLayout created with the descriptor-buffer bit, plus its total
// byte size and the byte offset of each binding (both queried from the driver).
class DescriptorSetLayout {
public:
    [[nodiscard]] static std::expected<DescriptorSetLayout, core::Error>
    create(const Device& device, const DescriptorBufferFunctions& fns,
           std::span<const DescriptorBinding> bindings);

    DescriptorSetLayout() = default;
    ~DescriptorSetLayout();
    DescriptorSetLayout(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout& operator=(DescriptorSetLayout&& other) noexcept;
    DescriptorSetLayout(const DescriptorSetLayout&) = delete;
    DescriptorSetLayout& operator=(const DescriptorSetLayout&) = delete;

    [[nodiscard]] VkDescriptorSetLayout handle() const { return layout_; }
    [[nodiscard]] VkDeviceSize size() const { return size_; }
    [[nodiscard]] VkDeviceSize binding_offset(std::uint32_t binding) const;

private:
    void destroy() noexcept;

    VkDevice              device_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout_ = VK_NULL_HANDLE;
    VkDeviceSize          size_ = 0;
    std::unordered_map<std::uint32_t, VkDeviceSize> offsets_;
};

// Host-visible buffer holding one set's worth of descriptors for a layout.
// Descriptors are written directly into mapped memory via vkGetDescriptorEXT,
// then bound at draw time with bind().
class DescriptorBuffer {
public:
    [[nodiscard]] static std::expected<DescriptorBuffer, core::Error>
    create(const Device& device, GpuAllocator& allocator,
           const DescriptorBufferFunctions& fns, const DescriptorSetLayout& layout);

    DescriptorBuffer() = default;

    void write_uniform_buffer(std::uint32_t binding, VkDeviceAddress address, VkDeviceSize range);
    void write_storage_buffer(std::uint32_t binding, VkDeviceAddress address, VkDeviceSize range);
    void write_sampled_image(std::uint32_t binding, VkImageView view, VkImageLayout layout);
    void write_storage_image(std::uint32_t binding, VkImageView view, VkImageLayout layout);
    void write_sampler(std::uint32_t binding, VkSampler sampler);

    [[nodiscard]] VkDeviceAddress device_address() const { return address_; }

    // Binds this descriptor buffer and points descriptor set index `set` at it.
    void bind(VkCommandBuffer cmd, VkPipelineBindPoint bind_point,
              VkPipelineLayout pipeline_layout, std::uint32_t set) const;

private:
    void write_buffer_descriptor(std::uint32_t binding, VkDescriptorType type,
                                 VkDeviceAddress address, VkDeviceSize range);

    const Device*                    device_ = nullptr;
    const DescriptorBufferFunctions* fns_ = nullptr;
    const DescriptorSetLayout*       layout_ = nullptr;
    GpuBuffer                        buffer_;
    VkDeviceAddress                  address_ = 0;
};

// Debug self-test: build a one-uniform-buffer layout, allocate a descriptor
// buffer, and write a descriptor into it. Verifies the EXT plumbing is clean.
[[nodiscard]] std::expected<void, core::Error>
run_descriptor_buffer_self_test(const Device& device, GpuAllocator& allocator);

} // namespace engine::rhi
