#include "engine/renderer/upscaler.hpp"
#include "engine/renderer/upscaler_settings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "engine/renderer/taa_pass.hpp"

namespace engine::renderer {

namespace {

struct ModeName {
    UpscalerMode     mode;
    std::string_view name;
    float            scale;
};
constexpr std::array<ModeName, 4> kModes{{
    {UpscalerMode::native_aa, "native_aa", 1.0F},
    {UpscalerMode::quality, "quality", 1.0F / 1.5F},
    {UpscalerMode::balanced, "balanced", 1.0F / 1.7F},
    {UpscalerMode::performance, "performance", 0.5F},
}};

struct BackendName {
    UpscalerBackend  backend;
    std::string_view name;
};
constexpr std::array<BackendName, 2> kBackends{{
    {UpscalerBackend::taau, "taau"},
    {UpscalerBackend::fsr2, "fsr2"},
}};

constexpr float kMinScale = 0.25F;

} // namespace

float upscaler_mode_scale(UpscalerMode mode) {
    for (const ModeName& m : kModes) {
        if (m.mode == mode) return m.scale;
    }
    return 1.0F;
}

float upscaler_render_scale(const UpscalerSettings& settings) {
    const float scale =
        settings.render_scale > 0.0F ? settings.render_scale : upscaler_mode_scale(settings.mode);
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
    for (const ModeName& m : kModes) {
        if (m.mode == mode) return m.name;
    }
    return "native_aa";
}

std::string_view to_string(UpscalerBackend backend) {
    for (const BackendName& b : kBackends) {
        if (b.backend == backend) return b.name;
    }
    return "taau";
}

std::optional<UpscalerMode> parse_upscaler_mode(std::string_view name) {
    for (const ModeName& m : kModes) {
        if (m.name == name) return m.mode;
    }
    return std::nullopt;
}

std::optional<UpscalerBackend> parse_upscaler_backend(std::string_view name) {
    for (const BackendName& b : kBackends) {
        if (b.name == name) return b.backend;
    }
    return std::nullopt;
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
