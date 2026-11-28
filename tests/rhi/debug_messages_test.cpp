// Validation messages reach Device::CreateInfo::on_debug_message (#113), and
// the test fixture's sink turns an error into a failure of the running test.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"

namespace {

using engine::rhi::DebugMessageSeverity;

[[nodiscard]] bool validation_layer_installed() {
    std::uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

struct Received {
    DebugMessageSeverity severity;
    std::string          message;
};

// Sends a message through the instance's messengers, as the layer would.
void submit(VkInstance instance, VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            const char* message) {
    const auto submit_message = reinterpret_cast<PFN_vkSubmitDebugUtilsMessageEXT>(
        vkGetInstanceProcAddr(instance, "vkSubmitDebugUtilsMessageEXT"));
    ASSERT_NE(submit_message, nullptr);
    VkDebugUtilsMessengerCallbackDataEXT data{};
    data.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT;
    data.pMessageIdName = "quat-test";
    data.pMessage = message;
    submit_message(instance, severity, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
}

TEST(ValidationReport, ErrorFailsTheRunningTestAndNamesIt) {
    EXPECT_NONFATAL_FAILURE(
        quat::test::report_validation_message(DebugMessageSeverity::error, "VUID-x boom"),
        "Vulkan validation error in ValidationReport.ErrorFailsTheRunningTestAndNamesIt: "
        "VUID-x boom");
}

TEST(ValidationReport, WarningIsNotAFailure) {
    quat::test::report_validation_message(DebugMessageSeverity::warning, "a performance hint");
    EXPECT_EQ(quat::test::validation_errors_outside_tests(), 0U);
}

// The sink receives warnings and errors with their severity, and keeps
// receiving them after the Device is moved (the messenger holds its address).
TEST(GpuDeviceDebugMessages, SinkReceivesMessagesAcrossMoves) {
    QUAT_REQUIRE_GPU(g);
    (void)g;
    if (!validation_layer_installed()) {
        GTEST_SKIP() << "VK_LAYER_KHRONOS_validation is not installed";
    }
    std::vector<Received> received;
    auto device = engine::rhi::Device::create({
        .enable_validation = true,
        .application_name = "engine_tests_debug_messages",
        .on_debug_message =
            [&received](DebugMessageSeverity severity, std::string_view message) {
                if (message.find("quat-test-") != std::string_view::npos) {
                    received.push_back({severity, std::string(message)});
                }
            },
    });
    ASSERT_OK(device);
    submit(device->instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "quat-test-error");
    submit(device->instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
           "quat-test-warning");

    engine::rhi::Device moved = std::move(*device);
    submit(moved.instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT, "quat-test-moved");

    ASSERT_EQ(received.size(), 3U);
    EXPECT_EQ(received[0].severity, DebugMessageSeverity::error);
    EXPECT_EQ(received[0].message, "quat-test-error");
    EXPECT_EQ(received[1].severity, DebugMessageSeverity::warning);
    EXPECT_EQ(received[1].message, "quat-test-warning");
    EXPECT_EQ(received[2].message, "quat-test-moved");
}

} // namespace
