#include "engine/rhi/queue_selection.hpp"

namespace engine::rhi {

std::optional<QueueFamilies>
select_queue_families(std::span<const VkQueueFamilyProperties> families,
                      const PresentSupportFn& can_present) {
    constexpr std::uint32_t none = VK_QUEUE_FAMILY_IGNORED;
    const auto count = static_cast<std::uint32_t>(families.size());
    const bool want_present = static_cast<bool>(can_present);

    std::uint32_t graphics = none, compute = none, transfer = none, present = none;
    std::uint32_t graphics_present = none; // first family that draws and presents
    std::uint32_t dedicated_compute = none, dedicated_transfer = none;

    for (std::uint32_t i = 0; i < count; ++i) {
        if (families[i].queueCount == 0) continue;
        const VkQueueFlags flags = families[i].queueFlags;
        const bool g = (flags & VK_QUEUE_GRAPHICS_BIT) != 0;
        const bool c = (flags & VK_QUEUE_COMPUTE_BIT) != 0;
        const bool t = (flags & VK_QUEUE_TRANSFER_BIT) != 0;

        if (g && graphics == none) graphics = i;
        if (c && compute == none) compute = i;
        if (t && transfer == none) transfer = i;
        // Prefer a compute family without graphics (async compute).
        if (c && !g && dedicated_compute == none) dedicated_compute = i;
        // Prefer a transfer-only family (dedicated DMA).
        if (t && !g && !c && dedicated_transfer == none) dedicated_transfer = i;

        // Ask about presentation once per family, and only while it can still
        // change the outcome (no graphics+present family found yet).
        if (want_present && graphics_present == none && can_present(i)) {
            if (present == none) present = i;
            if (g) graphics_present = i;
        }
    }

    if (graphics == none) return std::nullopt;
    if (want_present) {
        if (present == none) return std::nullopt;
        if (graphics_present != none) {
            graphics = graphics_present;
            present = graphics_present;
        }
    }
    if (dedicated_compute != none)  compute = dedicated_compute;
    if (dedicated_transfer != none) transfer = dedicated_transfer;
    // Graphics queues always support transfer; ensure a fallback.
    if (compute == none)  compute = graphics;
    if (transfer == none) transfer = graphics;

    return QueueFamilies{graphics, compute, transfer, present};
}

} // namespace engine::rhi
