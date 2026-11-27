#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include <vulkan/vulkan.h>

namespace engine::rhi {

// Queue family indices selected for the logical device. Compute and transfer
// fall back to the graphics family when no dedicated family exists. `present`
// is the family that presents to the window surface: the graphics family
// whenever that family can present, VK_QUEUE_FAMILY_IGNORED for a device
// created without a surface (headless tools and tests).
struct QueueFamilies {
    std::uint32_t graphics = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t compute  = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t transfer = VK_QUEUE_FAMILY_IGNORED;
    std::uint32_t present  = VK_QUEUE_FAMILY_IGNORED;
};

// Whether queue family `family` of the device being considered can present to
// the window surface (vkGetPhysicalDeviceSurfaceSupportKHR). Called at most
// once per family.
using PresentSupportFn = std::function<bool(std::uint32_t family)>;

// Picks queue families from a device's family properties. Without
// `can_present` (no surface) it picks the first graphics family and leaves
// `present` unset. With it, the graphics family is the first one that can
// both draw and present (one queue does both: no cross-family handoff); only
// when no family does both is a separate present family chosen. Returns
// nullopt when the device has no graphics family, or when a surface is given
// and no family can present to it.
[[nodiscard]] std::optional<QueueFamilies>
select_queue_families(std::span<const VkQueueFamilyProperties> families,
                      const PresentSupportFn& can_present);

} // namespace engine::rhi
