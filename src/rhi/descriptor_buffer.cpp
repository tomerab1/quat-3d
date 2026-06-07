#include "engine/rhi/descriptor_buffer.hpp"

#include <array>
#include <cstdio>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] VkDeviceAddress buffer_device_address(VkDevice device, VkBuffer buffer) {
    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(device, &info);
}

} // namespace

// ===========================================================================
// DescriptorBufferFunctions
// ===========================================================================
DescriptorBufferFunctions DescriptorBufferFunctions::load(VkDevice device) {
    DescriptorBufferFunctions f;
    f.get_layout_size = reinterpret_cast<PFN_vkGetDescriptorSetLayoutSizeEXT>(
        vkGetDeviceProcAddr(device, "vkGetDescriptorSetLayoutSizeEXT"));
    f.get_binding_offset = reinterpret_cast<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>(
        vkGetDeviceProcAddr(device, "vkGetDescriptorSetLayoutBindingOffsetEXT"));
    f.get_descriptor = reinterpret_cast<PFN_vkGetDescriptorEXT>(
        vkGetDeviceProcAddr(device, "vkGetDescriptorEXT"));
    f.cmd_bind_buffers = reinterpret_cast<PFN_vkCmdBindDescriptorBuffersEXT>(
        vkGetDeviceProcAddr(device, "vkCmdBindDescriptorBuffersEXT"));
    f.cmd_set_offsets = reinterpret_cast<PFN_vkCmdSetDescriptorBufferOffsetsEXT>(
        vkGetDeviceProcAddr(device, "vkCmdSetDescriptorBufferOffsetsEXT"));
    return f;
}

bool DescriptorBufferFunctions::valid() const {
    return get_layout_size && get_binding_offset && get_descriptor && cmd_bind_buffers
        && cmd_set_offsets;
}

// ===========================================================================
// DescriptorSetLayout
// ===========================================================================
std::expected<DescriptorSetLayout, core::Error>
DescriptorSetLayout::create(const Device& device, const DescriptorBufferFunctions& fns,
                            std::span<const DescriptorBinding> bindings) {
    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    vk_bindings.reserve(bindings.size());
    for (const DescriptorBinding& b : bindings) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding = b.binding;
        lb.descriptorType = b.type;
        lb.descriptorCount = b.count;
        lb.stageFlags = b.stages;
        vk_bindings.push_back(lb);
    }

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    info.bindingCount = static_cast<std::uint32_t>(vk_bindings.size());
    info.pBindings = vk_bindings.data();

    DescriptorSetLayout out;
    out.device_ = device.handle();
    if (VkResult r = vkCreateDescriptorSetLayout(out.device_, &info, nullptr, &out.layout_);
        r != VK_SUCCESS) {
        return fail("vkCreateDescriptorSetLayout (descriptor buffer) failed");
    }

    fns.get_layout_size(out.device_, out.layout_, &out.size_);
    for (const DescriptorBinding& b : bindings) {
        VkDeviceSize offset = 0;
        fns.get_binding_offset(out.device_, out.layout_, b.binding, &offset);
        out.offsets_[b.binding] = offset;
    }
    return out;
}

VkDeviceSize DescriptorSetLayout::binding_offset(std::uint32_t binding) const {
    if (auto it = offsets_.find(binding); it != offsets_.end()) {
        return it->second;
    }
    return 0;
}

void DescriptorSetLayout::destroy() noexcept {
    if (layout_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, layout_, nullptr);
    }
    device_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    size_ = 0;
    offsets_.clear();
}

DescriptorSetLayout::~DescriptorSetLayout() { destroy(); }

DescriptorSetLayout::DescriptorSetLayout(DescriptorSetLayout&& other) noexcept {
    *this = std::move(other);
}

DescriptorSetLayout& DescriptorSetLayout::operator=(DescriptorSetLayout&& other) noexcept {
    if (this != &other) {
        destroy();
        device_  = std::exchange(other.device_, VK_NULL_HANDLE);
        layout_  = std::exchange(other.layout_, VK_NULL_HANDLE);
        size_    = std::exchange(other.size_, 0);
        offsets_ = std::move(other.offsets_);
    }
    return *this;
}

// ===========================================================================
// DescriptorBuffer
// ===========================================================================
std::expected<DescriptorBuffer, core::Error>
DescriptorBuffer::create(const Device& device, GpuAllocator& allocator,
                         const DescriptorBufferFunctions& fns,
                         const DescriptorSetLayout& layout) {
    const VkDeviceSize size = layout.size() != 0 ? layout.size() : 1;
    const VkBufferUsageFlags usage =
        VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
        | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT
        | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    auto buffer = allocator.create_buffer(
        size, usage, VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!buffer) return std::unexpected(buffer.error());

    DescriptorBuffer out;
    out.device_ = &device;
    out.fns_ = &fns;
    out.layout_ = &layout;
    out.buffer_ = std::move(*buffer);
    out.address_ = buffer_device_address(device.handle(), out.buffer_.handle());
    return out;
}

void DescriptorBuffer::write_buffer_descriptor(std::uint32_t binding, VkDescriptorType type,
                                               VkDeviceAddress address, VkDeviceSize range) {
    const auto& props = device_->descriptor_buffer_properties();

    VkDescriptorAddressInfoEXT addr{};
    addr.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT;
    addr.address = address;
    addr.range = range;
    addr.format = VK_FORMAT_UNDEFINED;

    VkDescriptorGetInfoEXT get{};
    get.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
    get.type = type;
    std::size_t descriptor_size = 0;
    if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
        get.data.pUniformBuffer = &addr;
        descriptor_size = props.uniformBufferDescriptorSize;
    } else {
        get.data.pStorageBuffer = &addr;
        descriptor_size = props.storageBufferDescriptorSize;
    }

    auto* dst = static_cast<char*>(buffer_.mapped()) + layout_->binding_offset(binding);
    fns_->get_descriptor(device_->handle(), &get, descriptor_size, dst);
}

void DescriptorBuffer::write_uniform_buffer(std::uint32_t binding, VkDeviceAddress address,
                                            VkDeviceSize range) {
    write_buffer_descriptor(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, address, range);
}

void DescriptorBuffer::write_storage_buffer(std::uint32_t binding, VkDeviceAddress address,
                                            VkDeviceSize range) {
    write_buffer_descriptor(binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, address, range);
}

void DescriptorBuffer::write_sampled_image(std::uint32_t binding, VkImageView view,
                                           VkImageLayout layout) {
    const auto& props = device_->descriptor_buffer_properties();

    VkDescriptorImageInfo image{};
    image.sampler = VK_NULL_HANDLE;
    image.imageView = view;
    image.imageLayout = layout;

    VkDescriptorGetInfoEXT get{};
    get.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
    get.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    get.data.pSampledImage = &image;

    auto* dst = static_cast<char*>(buffer_.mapped()) + layout_->binding_offset(binding);
    fns_->get_descriptor(device_->handle(), &get, props.sampledImageDescriptorSize, dst);
}

void DescriptorBuffer::write_sampler(std::uint32_t binding, VkSampler sampler) {
    const auto& props = device_->descriptor_buffer_properties();

    VkDescriptorGetInfoEXT get{};
    get.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT;
    get.type = VK_DESCRIPTOR_TYPE_SAMPLER;
    get.data.pSampler = &sampler;

    auto* dst = static_cast<char*>(buffer_.mapped()) + layout_->binding_offset(binding);
    fns_->get_descriptor(device_->handle(), &get, props.samplerDescriptorSize, dst);
}

void DescriptorBuffer::bind(VkCommandBuffer cmd, VkPipelineBindPoint bind_point,
                            VkPipelineLayout pipeline_layout, std::uint32_t set) const {
    VkDescriptorBufferBindingInfoEXT binding_info{};
    binding_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT;
    binding_info.address = address_;
    binding_info.usage = VK_BUFFER_USAGE_RESOURCE_DESCRIPTOR_BUFFER_BIT_EXT
                       | VK_BUFFER_USAGE_SAMPLER_DESCRIPTOR_BUFFER_BIT_EXT;
    fns_->cmd_bind_buffers(cmd, 1, &binding_info);

    const std::uint32_t buffer_index = 0;
    const VkDeviceSize offset = 0;
    fns_->cmd_set_offsets(cmd, bind_point, pipeline_layout, set, 1, &buffer_index, &offset);
}

// ===========================================================================
// Self-test
// ===========================================================================
std::expected<void, core::Error>
run_descriptor_buffer_self_test(const Device& device, GpuAllocator& allocator) {
    const DescriptorBufferFunctions fns = DescriptorBufferFunctions::load(device.handle());
    if (!fns.valid()) {
        return fail("descriptor buffer EXT functions not all loadable");
    }

    const std::array<DescriptorBinding, 1> bindings = {DescriptorBinding{
        0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL}};
    auto layout = DescriptorSetLayout::create(device, fns, bindings);
    if (!layout) return std::unexpected(layout.error());
    if (layout->size() == 0) return fail("descriptor set layout reported zero size");

    auto ubo = allocator.create_buffer(
        64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!ubo) return std::unexpected(ubo.error());
    const VkDeviceAddress ubo_address = buffer_device_address(device.handle(), ubo->handle());

    auto descriptor_buffer = DescriptorBuffer::create(device, allocator, fns, *layout);
    if (!descriptor_buffer) return std::unexpected(descriptor_buffer.error());
    descriptor_buffer->write_uniform_buffer(0, ubo_address, 64);

    std::fprintf(stderr, "[selftest] descriptor buffer: layout size=%llu, uniform descriptor written\n",
                 static_cast<unsigned long long>(layout->size()));
    return {};
}

} // namespace engine::rhi
