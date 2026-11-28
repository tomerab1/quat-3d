#pragma once

// Photometric calibration of the renderer and the auto-exposure meter
// (ROADMAP R22.2.1). Graphics-free: the exposure shaders mirror these
// functions, and the tests check both against each other.
//
// One HDR unit of luminance is k_nits_per_unit cd/m². The sky-view LUT is
// computed for an extra-atmospheric sun of irradiance 1 and drawn × 8
// (lighting.slang SKY_LUT_BRIGHTNESS), so 8 units stand for the
// extra-atmospheric solar illuminance; everything in the sky (stars, Moon,
// ground) is placed on that one scale, from the lux SkyState reports.
//
// Exposure follows Lagarde & de Rousiers, "Moving Frostbite to Physically
// Based Rendering" (2014), section 5.1 (physical_camera.hpp): a luminance of
// 1.2 · 2^EV100 cd/m² maps to 1.0. HDR targets are *pre-exposed* (the same
// paper, and Unreal's PreExposure): every pass that writes light multiplies
// it by the frame's pre-exposure, the previous frame's exposure, so RGBA16F
// keeps its precision from 10⁻⁴ lux nights to 10⁵ lux noons.
//
// Auto exposure meters a 256-bin log2-luminance histogram (Opsenica 2019,
// the standard histogram metering of Unreal's eye adaptation): the mean
// log2 luminance of the pixels between two percentiles, smoothed over time
// in the log domain, and mapped to the display by an adaptive key —
// Krawczyk, Myszkowski & Seidel, "Lightness Perception in Tone Reproduction
// for High Dynamic Range Images" (Eurographics 2005), eq. 11:
//   key(L) = 1.03 - 2 / (2 + log10(L + 1)),  L in cd/m²,
// normalised to the user key at k_adaptive_key_reference_nits so daylight
// scenes keep the configured key and dim scenes are shown dimmer (a night
// stays a night).

#include <array>
#include <cstdint>
#include <span>

namespace engine::renderer::photometry {

// Extra-atmospheric solar illuminance at 1 au (≈ 128 klux; R22.1's
// SkyState::sun_illuminance_lux varies ±3.4 % around it over the year).
inline constexpr float k_sun_reference_lux = 128000.0F;
// lighting.slang / ibl_bake_env.slang SKY_LUT_BRIGHTNESS.
inline constexpr float k_sky_lut_scale = 8.0F;
// cd/m² per HDR unit (and lux per unit of illuminance).
inline constexpr float k_nits_per_unit = k_sun_reference_lux / k_sky_lut_scale;

// Illuminance of a clear moonless night sky on the ground: integrated
// starlight, airglow and zodiacal light, ~10⁻³ lux (Jensen, Durand, Stark,
// Premože, Dorsey & Shirley, "A Physically-Based Night Sky Model",
// SIGGRAPH 2001). A constant floor until R22.2.4 models airglow and
// zodiacal light.
inline constexpr float k_night_sky_floor_lux = 1.0e-3F;

// Reflected-light meter calibration constant K (ISO 2720; Lagarde 2014).
inline constexpr float k_meter_calibration = 12.5F;

// EV100 that exposes a luminance of `nits` to middle grey (log2(L·S/K) with
// S = 100).
[[nodiscard]] float ev100_from_luminance(float nits);
// The linear exposure, in HDR units, of an EV100: k_nits_per_unit /
// (1.2 · 2^ev100).
[[nodiscard]] float exposure_from_ev100_units(float ev100);

// ---- Histogram metering ----------------------------------------------------
inline constexpr std::uint32_t k_histogram_bins = 256;
// log2(HDR units) covered by bins 1..255 (bin 0 holds black, NaN and
// anything below the range): 40 EV, from ~4·10⁻⁶ cd/m² to ~4·10⁶ cd/m².
inline constexpr float k_histogram_min_log2 = -32.0F;
inline constexpr float k_histogram_max_log2 = 8.0F;

// The bin (0..255) of a (true, not pre-exposed) luminance in HDR units.
[[nodiscard]] std::uint32_t histogram_bin(float luminance_units);
// The log2 luminance at the centre of bin `bin` (1..255).
[[nodiscard]] float histogram_bin_log2(std::uint32_t bin);

// Centre-weighted metering (a camera's centre-weighted average metering;
// Unreal's metering mask): each pixel's histogram weight is
// 1 - s + s * exp(-r^2 / (2 * 0.4^2)), r the distance from the frame's
// centre normalised so the corners are at 1 (uv in [0, 1]^2). s = 0 meters
// the frame evenly. The histogram stores weights in fixed point,
// k_metering_weight_one per full-weight pixel.
inline constexpr float k_metering_weight_one = 256.0F;
[[nodiscard]] float metering_weight(float u, float v, float strength);

struct MeteringParams {
    float low_percent  = 0.10F; // pixels darker than this fraction are ignored
    float high_percent = 0.90F; // pixels brighter than this fraction are ignored
};
// Mean log2 luminance (HDR units) of the pixels between the two percentiles
// of the non-black bins; NaN when every pixel is black.
[[nodiscard]] float metered_log2(std::span<const std::uint32_t, k_histogram_bins> bins,
                                 const MeteringParams& params = {});

// When a frame holds nothing measurable (all black: the scene may be darker
// than the pre-exposure lets RGBA16F hold), exposure_average.slang jumps the
// adapted luminance this many EV below the darkest luminance the frame could
// have shown; the EV100 clamp bounds the climb.
inline constexpr float k_underexposed_step_ev = 4.0F;

// Exponential adaptation in the log domain: `previous` moved toward
// `target` with time constant `tau` over `dt` seconds.
[[nodiscard]] float adapt_log2(float previous, float target, float dt, float tau);

// Krawczyk et al. 2005's key value at a luminance in cd/m².
[[nodiscard]] float krawczyk_key(float nits);
inline constexpr float k_adaptive_key_reference_nits = 1000.0F;
// The key the adapted luminance maps to: `user_key` scaled by
// key(L) / key(reference), never above `user_key`; `user_key` itself when
// `adaptive` is off.
[[nodiscard]] float adaptive_key(float adapted_units, float user_key, bool adaptive);

// The auto exposure (HDR units, before compensation) of an adapted
// luminance, clamped to the EV100 range [min_ev100, max_ev100].
[[nodiscard]] float auto_exposure(float adapted_units, float user_key, bool adaptive,
                                  float min_ev100, float max_ev100);

} // namespace engine::renderer::photometry
