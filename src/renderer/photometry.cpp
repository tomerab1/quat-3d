#include "engine/renderer/photometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/renderer/physical_camera.hpp"

namespace engine::renderer::photometry {

namespace {

constexpr float k_log_range = k_histogram_max_log2 - k_histogram_min_log2;
constexpr float k_bin_width = k_log_range / static_cast<float>(k_histogram_bins - 1);

} // namespace

float ev100_from_luminance(float nits) {
    return std::log2(std::max(nits, 1e-30F) * 100.0F / k_meter_calibration);
}

float exposure_from_ev100_units(float ev100) {
    return k_nits_per_unit * exposure_from_ev100(ev100);
}

std::uint32_t histogram_bin(float luminance_units) {
    // NaN fails the comparison and lands in bin 0 with black.
    if (!(luminance_units >= std::exp2(k_histogram_min_log2))) return 0;
    const float t = (std::log2(luminance_units) - k_histogram_min_log2) / k_log_range;
    const float scaled = std::clamp(t, 0.0F, 1.0F) * static_cast<float>(k_histogram_bins - 1);
    return 1U + std::min(static_cast<std::uint32_t>(scaled), k_histogram_bins - 2);
}

float histogram_bin_log2(std::uint32_t bin) {
    return k_histogram_min_log2 + (static_cast<float>(bin) - 0.5F) * k_bin_width;
}

float metering_weight(float u, float v, float strength) {
    const float s = std::clamp(strength, 0.0F, 1.0F);
    const float du = u - 0.5F;
    const float dv = v - 0.5F;
    const float r2 = (du * du + dv * dv) * 2.0F;
    return 1.0F - s + s * std::exp(-r2 / (2.0F * 0.4F * 0.4F));
}

float metered_log2(std::span<const std::uint32_t, k_histogram_bins> bins,
                   const MeteringParams& params) {
    float total = 0.0F;
    for (std::uint32_t b = 1; b < k_histogram_bins; ++b) total += static_cast<float>(bins[b]);
    if (total <= 0.0F) return std::numeric_limits<float>::quiet_NaN();
    const float lo_p = std::clamp(params.low_percent, 0.0F, 1.0F);
    const float hi_p = std::clamp(params.high_percent, lo_p, 1.0F);
    const float lo = total * lo_p;
    // A window of at least one pixel's weight, so equal percentiles pick a pixel.
    const float hi = std::max(total * hi_p, std::min(lo + k_metering_weight_one, total));
    float below = 0.0F;
    float weight = 0.0F;
    float sum = 0.0F;
    for (std::uint32_t b = 1; b < k_histogram_bins; ++b) {
        const float count = static_cast<float>(bins[b]);
        const float overlap = std::max(std::min(below + count, hi) - std::max(below, lo), 0.0F);
        weight += overlap;
        sum += overlap * histogram_bin_log2(b);
        below += count;
    }
    return sum / std::max(weight, 1e-6F);
}

float adapt_log2(float previous, float target, float dt, float tau) {
    if (!std::isfinite(previous)) return target;
    if (!std::isfinite(target)) return previous;
    const float rate = 1.0F - std::exp(-std::max(dt, 0.0F) / std::max(tau, 1e-3F));
    return previous + (target - previous) * rate;
}

float krawczyk_key(float nits) {
    return 1.03F - 2.0F / (2.0F + std::log10(std::max(nits, 0.0F) + 1.0F));
}

float adaptive_key(float adapted_units, float user_key, bool adaptive) {
    if (!adaptive) return user_key;
    const float ratio = krawczyk_key(adapted_units * k_nits_per_unit) /
                        krawczyk_key(k_adaptive_key_reference_nits);
    return user_key * std::clamp(ratio, 0.0F, 1.0F);
}

float auto_exposure(float adapted_units, float user_key, bool adaptive, float min_ev100,
                    float max_ev100) {
    const float lo_ev = std::min(min_ev100, max_ev100);
    const float hi_ev = std::max(min_ev100, max_ev100);
    const float e = adaptive_key(adapted_units, user_key, adaptive) /
                    std::max(adapted_units, 1e-30F);
    // A higher EV100 means a brighter scene, i.e. a smaller exposure.
    return std::clamp(e, exposure_from_ev100_units(hi_ev), exposure_from_ev100_units(lo_ev));
}

} // namespace engine::renderer::photometry
