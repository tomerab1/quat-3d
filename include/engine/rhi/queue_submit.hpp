#pragma once

// One-time queue work (bakes, uploads): submit, then wait for the queue to
// go idle, with both results checked (issue #110). A lost device is named as
// such in the error, so a bake that fails because a shader took the GPU down
// says so instead of reporting a generic submit failure.

#include <expected>
#include <string>
#include <string_view>

#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"

namespace engine::rhi {

// "<what>: GPU device lost (<call> returned -4)", or
// "<what>: <call> failed (<result>)" for any other failure.
[[nodiscard]] std::string queue_error_message(std::string_view what, std::string_view call,
                                              VkResult result);

// vkQueueSubmit2(queue, submit) then vkQueueWaitIdle(queue). The command
// buffers may be freed once this returns, whatever the outcome: a failed
// submit queued nothing, and a failed wait means the device is lost.
[[nodiscard]] std::expected<void, core::Error>
submit_and_wait_idle(VkQueue queue, const VkSubmitInfo2& submit, std::string_view what);

} // namespace engine::rhi
