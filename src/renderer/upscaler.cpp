#include "engine/renderer/upscaler.hpp"
#include "engine/renderer/upscaler_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

#include "engine/renderer/taa_pass.hpp"

namespace engine::renderer {

namespace {

// Render scale per axis of each mode, in enum order.
constexpr std::array<float, upscaler_mode_names.size()> kModeScale{1.0F, 1.0F / 1.5F,
                                                                  1.0F / 1.7F, 0.5F};
constexpr float kMinScale = 0.25F;

template <typename Enum, std::size_t N>
[[nodiscard]] Enum clamp_index(int value) {
    return static_cast<Enum>(std::clamp(value, 0, static_cast<int>(N) - 1));
}

} // namespace

UpscalerMode upscaler_mode(const UpscalerSettings& settings) {
    return clamp_index<UpscalerMode, upscaler_mode_names.size()>(settings.mode);
}

UpscalerBackend upscaler_backend(const UpscalerSettings& settings) {
    return clamp_index<UpscalerBackend, upscaler_backend_names.size()>(settings.backend);
}

float upscaler_mode_scale(UpscalerMode mode) {
    const auto i = static_cast<std::size_t>(mode);
    return i < kModeScale.size() ? kModeScale[i] : 1.0F;
}

float upscaler_render_scale(const UpscalerSettings& settings) {
    const float scale = settings.render_scale > 0.0F ? settings.render_scale
                                                     : upscaler_mode_scale(upscaler_mode(settings));
    return std::isfinite(scale) ? std::clamp(scale, kMinScale, 1.0F) : 1.0F;
}

VkExtent2D upscaler_render_extent(VkExtent2D display, const UpscalerSettings& settings) {
    const float scale = upscaler_render_scale(settings);
    const auto axis = [scale](std::uint32_t d) {
        if (d == 0) return 0U;
        return std::max(1U, static_cast<std::uint32_t>(std::lround(static_cast<float>(d) * scale)));
    };
    return {axis(display.width), axis(display.height)};
}

std::string_view to_string(UpscalerMode mode) {
    const auto i = static_cast<std::size_t>(mode);
    return i < upscaler_mode_names.size() ? upscaler_mode_names[i] : upscaler_mode_names[0];
}

std::string_view to_string(UpscalerBackend backend) {
    const auto i = static_cast<std::size_t>(backend);
    return i < upscaler_backend_names.size() ? upscaler_backend_names[i]
                                             : upscaler_backend_names[0];
}

std::optional<UpscalerMode> parse_upscaler_mode(std::string_view name) {
    for (std::size_t i = 0; i < upscaler_mode_names.size(); ++i) {
        if (upscaler_mode_names[i] == name) return static_cast<UpscalerMode>(i);
    }
    return std::nullopt;
}

std::optional<UpscalerBackend> parse_upscaler_backend(std::string_view name) {
    for (std::size_t i = 0; i < upscaler_backend_names.size(); ++i) {
        if (upscaler_backend_names[i] == name) return static_cast<UpscalerBackend>(i);
    }
    return std::nullopt;
}

void apply_taa_settings(UpscalerInputs& in, const TaaSettings& taa) {
    in.feedback = taa.feedback;
    if (!taa.enabled) in.reset = true;
}

std::expected<std::unique_ptr<Upscaler>, core::Error>
create_upscaler(UpscalerBackend backend, const rhi::Device& device,
                rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
                const std::string& cooked_shader_dir, std::uint32_t frames_in_flight) {
    switch (backend) {
    case UpscalerBackend::taau: {
        auto taau =
            TaaUpscaler::create(device, allocator, cache, cooked_shader_dir, frames_in_flight);
        if (!taau) return std::unexpected(taau.error());
        return std::unique_ptr<Upscaler>(std::move(*taau));
    }
    case UpscalerBackend::fsr2:
        break;
    }
    return std::unexpected(core::Error{
        "upscaler: the FSR 2 backend is not built yet (see docs/upscaling.md)"});
}

} // namespace engine::renderer
