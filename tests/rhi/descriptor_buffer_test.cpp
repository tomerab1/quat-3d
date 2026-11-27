// Descriptor buffer plumbing: EXT entry points load, a one-uniform-buffer
// layout reports its size, and a descriptor buffer accepts a UBO write.

#include <array>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using namespace engine::rhi;

TEST(GpuDescriptorBuffer, WritesAUniformBufferDescriptor) {
    QUAT_REQUIRE_GPU(g);
    const DescriptorBufferFunctions fns = DescriptorBufferFunctions::load(g->device.handle());
    ASSERT_TRUE(fns.valid()) << "descriptor buffer EXT functions not all loadable";

    const std::array<DescriptorBinding, 1> bindings = {
        DescriptorBinding{0, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL}};
    auto layout = DescriptorSetLayout::create(g->device, fns, bindings);
    ASSERT_OK(layout);
    // The classic-sets fallback layout has no byte size (the driver owns the
    // storage); the size checks only apply to the descriptor-buffer path.
    if (!layout->is_classic()) {
        EXPECT_GT(layout->size(), 0U) << "layout reported zero size";
        EXPECT_LT(layout->binding_offset(0), layout->size());
    }

    auto ubo = g->allocator.create_buffer(
        64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(ubo);
    const VkDeviceAddress ubo_address = buffer_device_address(g->device.handle(), *ubo);
    ASSERT_NE(ubo_address, 0U);

    auto descriptor_buffer = DescriptorBuffer::create(g->device, g->allocator, fns, *layout);
    ASSERT_OK(descriptor_buffer);
    if (!layout->is_classic()) {
        EXPECT_NE(descriptor_buffer->device_address(), 0U);
    }
    descriptor_buffer->write_uniform_buffer(0, ubo_address, 64);
}

} // namespace
