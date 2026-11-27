// Queue family selection and presentation support (issues #5, #9).
//
// select_queue_families() is a pure function over queue-family properties and
// a present-support callback, so the selection rules are tested here with
// injected families. The Gpu* tests check the surface-less test device and a
// real surface (VK_EXT_headless_surface) end to end: device selection,
// swapchain creation, a render graph ending in the PRESENT_SRC transition and
// a present on Device::present_queue(), clean under synchronization
// validation.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/core/log.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/queue_selection.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/swapchain.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::PresentSupportFn;
using engine::rhi::QueueFamilies;
using engine::rhi::select_queue_families;

constexpr std::uint32_t k_none = VK_QUEUE_FAMILY_IGNORED;
constexpr VkQueueFlags k_gct = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
constexpr VkQueueFlags k_ct = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
constexpr VkQueueFlags k_t = VK_QUEUE_TRANSFER_BIT;

[[nodiscard]] std::vector<VkQueueFamilyProperties> families(std::initializer_list<VkQueueFlags> flags) {
    std::vector<VkQueueFamilyProperties> out;
    for (const VkQueueFlags f : flags) {
        VkQueueFamilyProperties p{};
        p.queueFlags = f;
        p.queueCount = 1;
        out.push_back(p);
    }
    return out;
}

// A present-support callback answering from a fixed set, counting its calls.
struct PresentSet {
    explicit PresentSet(std::set<std::uint32_t> families = {}) : presenting(std::move(families)) {}
    std::set<std::uint32_t> presenting;
    std::vector<std::uint32_t> asked;
    [[nodiscard]] PresentSupportFn fn() {
        return [this](std::uint32_t family) {
            asked.push_back(family);
            return presenting.contains(family);
        };
    }
};

// Surface-less (tests, tools): first graphics family, dedicated compute and
// transfer families when they exist, and no present family.
TEST(QueueSelection, WithoutASurfaceNoPresentFamilyIsChosen) {
    const auto props = families({k_gct, k_ct, k_t});
    const auto q = select_queue_families(props, PresentSupportFn{});
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->graphics, 0U);
    EXPECT_EQ(q->compute, 1U);
    EXPECT_EQ(q->transfer, 2U);
    EXPECT_EQ(q->present, k_none);
}

// The usual case: the first graphics family presents, and is both.
TEST(QueueSelection, GraphicsFamilyThatPresentsIsUsedForBoth) {
    const auto props = families({k_gct, k_ct});
    PresentSet present{{0, 1}};
    const auto q = select_queue_families(props, present.fn());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->graphics, 0U);
    EXPECT_EQ(q->present, 0U);
    EXPECT_EQ(q->compute, 1U);
    // Stops asking once a family does both.
    EXPECT_EQ(present.asked, (std::vector<std::uint32_t>{0}));
}

// A graphics family that can present is preferred over an earlier graphics
// family that cannot (and over an earlier non-graphics presenting family).
TEST(QueueSelection, PrefersAGraphicsFamilyThatPresents) {
    const auto props = families({k_gct, k_ct, k_gct});
    PresentSet present{{1, 2}};
    const auto q = select_queue_families(props, present.fn());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->graphics, 2U);
    EXPECT_EQ(q->present, 2U);
    EXPECT_EQ(q->compute, 1U);
}

// No family both draws and presents: a separate present family is chosen
// (the swapchain then shares its images between the two families).
TEST(QueueSelection, FallsBackToASeparatePresentFamily) {
    const auto props = families({k_gct, k_ct});
    PresentSet present{{1}};
    const auto q = select_queue_families(props, present.fn());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->graphics, 0U);
    EXPECT_EQ(q->present, 1U);
    // Each family is asked at most once.
    EXPECT_EQ(present.asked, (std::vector<std::uint32_t>{0, 1}));
}

// A device with a surface but no family able to present to it is rejected.
TEST(QueueSelection, RejectsADeviceThatCannotPresent) {
    const auto props = families({k_gct, k_ct, k_t});
    PresentSet present{};
    EXPECT_FALSE(select_queue_families(props, present.fn()).has_value());
    // The same device is fine without a surface.
    EXPECT_TRUE(select_queue_families(props, PresentSupportFn{}).has_value());
}

// No graphics family: rejected, with or without presentation.
TEST(QueueSelection, RejectsADeviceWithoutGraphics) {
    const auto props = families({k_ct, k_t});
    PresentSet present{{0, 1}};
    EXPECT_FALSE(select_queue_families(props, present.fn()).has_value());
    EXPECT_FALSE(select_queue_families(props, PresentSupportFn{}).has_value());
}

// Families exposing no queues are never selected.
TEST(QueueSelection, IgnoresFamiliesWithoutQueues) {
    auto props = families({k_gct, k_gct});
    props[0].queueCount = 0;
    PresentSet present{{0, 1}};
    const auto q = select_queue_families(props, present.fn());
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(q->graphics, 1U);
    EXPECT_EQ(q->present, 1U);
}

// The shared test device is surface-less: no present queue, no surface, and
// a swapchain cannot be created from it.
TEST(GpuDevicePresent, SurfacelessDeviceHasNoPresentQueue) {
    QUAT_REQUIRE_GPU(g);
    EXPECT_EQ(g->device.queue_families().present, k_none);
    EXPECT_EQ(g->device.present_queue(), VK_NULL_HANDLE);
    EXPECT_EQ(g->device.surface(), VK_NULL_HANDLE);
    EXPECT_ERR(engine::rhi::Swapchain::create(g->device, VK_NULL_HANDLE, VkExtent2D{64, 64}));
}

// Portable setenv / unsetenv (an empty value unsets on Windows).
void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

[[nodiscard]] bool validation_layer_installed() {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    return std::any_of(layers.begin(), layers.end(), [](const VkLayerProperties& l) {
        return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
    });
}

[[nodiscard]] bool instance_extension_available(const char* name) {
    std::uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    return std::any_of(exts.begin(), exts.end(), [name](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

// With a surface, Device::create picks a device and family that can present
// to it, and the swapchain built on it presents from present_queue(). The
// frame is a real render graph ending in the backbuffer's PRESENT_SRC
// transition; synchronization validation must report nothing (issue #9: the
// transition used to have a NONE second scope, so present raced it).
TEST(GpuDevicePresent, HeadlessSurfaceSelectsAPresentQueueAndPresents) {
    QUAT_REQUIRE_GPU(g);
    if (!instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        GTEST_SKIP() << "VK_EXT_headless_surface is not available";
    }
    const std::array<const char*, 2> instance_exts{VK_KHR_SURFACE_EXTENSION_NAME,
                                                   VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    // Always under validation with synchronization validation (when the layer
    // is installed): the layer reads VK_LAYER_ENABLES at instance creation,
    // so it is set only around this device's creation and then restored.
    const std::uint64_t log_start = engine::core::log_ring().last_seq();
    const char* previous_enables = std::getenv("VK_LAYER_ENABLES");
    const std::optional<std::string> saved_enables =
        previous_enables != nullptr ? std::optional<std::string>(previous_enables) : std::nullopt;
    set_env("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
    auto device = engine::rhi::Device::create({
        .instance_extensions = instance_exts,
        .enable_validation = true,
        .application_name = "engine_tests_present",
        .create_surface = [](VkInstance instance)
            -> std::expected<VkSurfaceKHR, engine::core::Error> {
            const auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
            if (create == nullptr) {
                return std::unexpected(engine::core::Error{"no vkCreateHeadlessSurfaceEXT"});
            }
            VkHeadlessSurfaceCreateInfoEXT info{};
            info.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (create(instance, &info, nullptr, &surface) != VK_SUCCESS) {
                return std::unexpected(engine::core::Error{"vkCreateHeadlessSurfaceEXT failed"});
            }
            return surface;
        },
    });
    set_env("VK_LAYER_ENABLES", saved_enables ? saved_enables->c_str() : nullptr);
    ASSERT_OK(device);
    const QueueFamilies& families = device->queue_families();
    ASSERT_NE(device->surface(), VK_NULL_HANDLE);
    ASSERT_NE(families.present, k_none);
    ASSERT_NE(device->present_queue(), VK_NULL_HANDLE);
    VkBool32 supported = VK_FALSE;
    ASSERT_EQ(vkGetPhysicalDeviceSurfaceSupportKHR(device->physical_device(), families.present,
                                                   device->surface(), &supported),
              VK_SUCCESS);
    EXPECT_EQ(supported, VK_TRUE);

    auto swapchain = engine::rhi::Swapchain::create(*device, device->surface(), {64, 64});
    ASSERT_OK(swapchain);
    const VkDevice vk = device->handle();

    // Acquire an image, move it to PRESENT_SRC on the graphics queue, then
    // present it from the present queue (validation checks the queue/surface).
    VkSemaphoreCreateInfo sem_info{};
    sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateSemaphore(vk, &sem_info, nullptr, &acquired), VK_SUCCESS);
    ASSERT_EQ(vkCreateSemaphore(vk, &sem_info, nullptr, &rendered), VK_SUCCESS);
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = families.graphics;
    VkCommandPool pool = VK_NULL_HANDLE;
    ASSERT_EQ(vkCreateCommandPool(vk, &pool_info, nullptr, &pool), VK_SUCCESS);

    std::uint32_t index = 0;
    ASSERT_EQ(vkAcquireNextImageKHR(vk, swapchain->handle(), UINT64_MAX, acquired,
                                    VK_NULL_HANDLE, &index),
              VK_SUCCESS);

    VkCommandBufferAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc.commandPool = pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    ASSERT_EQ(vkAllocateCommandBuffers(vk, &alloc, &cmd), VK_SUCCESS);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    // A real render graph, as the frame builds it: the acquired image is
    // imported (first barrier waits at the acquire stage), written by a pass,
    // and left in PRESENT_SRC by the graph's final barrier, whose second scope
    // is the stage the render-finished semaphore is signalled at (issue #9).
    constexpr VkPipelineStageFlags2 k_acquire_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    constexpr VkPipelineStageFlags2 k_signal_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    engine::rhi::RenderGraph graph;
    const auto backbuffer = graph.import_image(
        "swapchain", swapchain->images()[index], swapchain->image_views()[index],
        swapchain->format(), swapchain->extent(), VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, k_acquire_stage, 0, k_signal_stage);
    (void)graph.add_pass("clear", engine::rhi::PassType::transfer)
        .writes(backbuffer, engine::rhi::ResourceUsage::transfer_dst)
        .execute([backbuffer](engine::rhi::PassContext& ctx) {
            const VkClearColorValue color{{0.2F, 0.4F, 0.6F, 1.0F}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(ctx.cmd(), ctx.resolve(backbuffer).image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        });
    ASSERT_OK(graph.compile());
    ASSERT_EQ(graph.planned_final_barriers().size(), 1U);
    EXPECT_EQ(graph.planned_final_barriers()[0].dstStageMask, k_signal_stage);
    graph.execute(cmd);
    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = acquired;
    wait.stageMask = k_acquire_stage;
    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = rendered;
    signal.stageMask = k_signal_stage;
    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    EXPECT_EQ(vkQueueSubmit2(device->graphics_queue(), 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);

    const VkSwapchainKHR sc = swapchain->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &index;
    const VkResult presented = vkQueuePresentKHR(device->present_queue(), &present);
    EXPECT_TRUE(presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR) << presented;

    vkDeviceWaitIdle(vk);
    vkDestroyCommandPool(vk, pool, nullptr);
    vkDestroySemaphore(vk, rendered, nullptr);
    vkDestroySemaphore(vk, acquired, nullptr);
    *swapchain = engine::rhi::Swapchain{}; // before the device destroys the surface

    // The validation layer (with synchronization validation) reported
    // nothing: present is ordered after the graph's PRESENT_SRC transition.
    engine::core::LogQuery query;
    query.since_seq = log_start;
    query.min_level = engine::core::LogLevel::warn;
    query.category = "vulkan";
    const auto messages = engine::core::log_ring().query(query);
    for (const auto& line : messages.lines) ADD_FAILURE() << line.message;
    if (!validation_layer_installed()) {
        std::printf("[ note ] validation layer not installed: message check is vacuous\n");
    }
}

} // namespace
