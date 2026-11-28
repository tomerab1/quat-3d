#include "engine/renderer/jitter.hpp"

#include <algorithm>
#include <cmath>

namespace engine::renderer {

namespace {

[[nodiscard]] float halton(std::uint64_t i, std::uint32_t base) {
    float f = 1.0F;
    float r = 0.0F;
    while (i > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(i % base);
        i /= base;
    }
    return r;
}

constexpr std::uint32_t k_native_phases = 8;
constexpr std::uint32_t k_max_phases = 128;

} // namespace

std::uint32_t jitter_phase_count(VkExtent2D render, VkExtent2D display) {
    if (render.width == 0 || render.height == 0) return k_native_phases;
    const float ratio = static_cast<float>(display.width) / static_cast<float>(render.width);
    const float phases = static_cast<float>(k_native_phases) * std::max(ratio * ratio, 1.0F);
    return std::clamp(static_cast<std::uint32_t>(std::ceil(phases)), k_native_phases,
                      k_max_phases);
}

ViewJitter halton_jitter(std::uint64_t frame, VkExtent2D render, std::uint32_t phase_count) {
    ViewJitter j;
    if (render.width == 0 || render.height == 0) return j;
    // Index 0 of the sequence is (0, 0); start at 1 so the cycle is centred.
    const std::uint64_t index = frame % std::max(phase_count, 1U) + 1;
    j.pixels = {halton(index, 2) - 0.5F, halton(index, 3) - 0.5F};
    j.ndc = {j.pixels.x * 2.0F / static_cast<float>(render.width),
             j.pixels.y * 2.0F / static_cast<float>(render.height)};
    return j;
}

glm::mat4 jitter_view_proj(const glm::mat4& view_proj, glm::vec2 ndc) {
    glm::mat4 t(1.0F);
    t[3][0] = ndc.x;
    t[3][1] = ndc.y;
    return t * view_proj;
}

float texture_lod_bias(VkExtent2D render, VkExtent2D display) {
    if (render.width == 0 || display.width == 0 || render.width >= display.width) return 0.0F;
    return std::log2(static_cast<float>(render.width) / static_cast<float>(display.width));
}

} // namespace engine::renderer
