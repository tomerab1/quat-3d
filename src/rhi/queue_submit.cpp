#include "engine/rhi/queue_submit.hpp"

namespace engine::rhi {

std::string queue_error_message(std::string_view what, std::string_view call, VkResult result) {
    const std::string code = std::to_string(static_cast<int>(result));
    if (result == VK_ERROR_DEVICE_LOST) {
        return std::string(what) + ": GPU device lost (" + std::string(call) + " returned " +
               code + ")";
    }
    return std::string(what) + ": " + std::string(call) + " failed (" + code + ")";
}

std::expected<void, core::Error> submit_and_wait_idle(VkQueue queue, const VkSubmitInfo2& submit,
                                                      std::string_view what) {
    if (const VkResult r = vkQueueSubmit2(queue, 1, &submit, VK_NULL_HANDLE); r != VK_SUCCESS) {
        return std::unexpected(core::Error{queue_error_message(what, "vkQueueSubmit2", r)});
    }
    if (const VkResult r = vkQueueWaitIdle(queue); r != VK_SUCCESS) {
        return std::unexpected(core::Error{queue_error_message(what, "vkQueueWaitIdle", r)});
    }
    return {};
}

} // namespace engine::rhi
