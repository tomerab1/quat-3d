#pragma once

// HdriEnvironment — image-based lighting from an HDRI panorama (ROADMAP R21).
//
// set_image() uploads a decoded equirectangular .hdr (RGBA32F, read with
// Load so values beyond half-float range survive) and finds its sun
// (hdri_sun.hpp). add_bake_to_graph() then records, in one compute pass,
// the whole precompute whenever the image or the bake settings changed:
//
//   1. hdri_to_cube      panorama -> environment cube mip 0 (rotation about
//                        +Y and intensity applied, the sun optionally
//                        clamped out), 2x2 supersampled;
//   2. cube_downsample   the environment's full mip chain (box filter);
//   3. env_prefilter     the prefiltered specular cube, one mip per
//                        roughness step, GGX importance sampling with
//                        filtered importance sampling (Křivánek & Colbert
//                        2008) over the mip chain — noise-free with 64
//                        samples even around a bright sun;
//   4. sh_project +      diffuse irradiance as SH9 (Ramamoorthi & Hanrahan
//      sh_irradiance     2001), exact texel solid angles, evaluated into the
//                        small irradiance cube the lighting pass samples;
//   5. ibl_brdf_lut      the split-sum DFG LUT (once).
//
// views() is a drop-in IblViewSet (irradiance, prefiltered, BRDF LUT) for
// the lighting and transparent passes, and environment_view() the full
// cube for the sky background. Maps are persistent images outside the
// graph's tracking: the pass carries its own sync2 barriers (an execution
// dependency on everything earlier in the queue first, so frames still
// sampling the previous bake finish before it is overwritten) and leaves
// every map in SHADER_READ_ONLY_OPTIMAL for compute and fragment readers.
// Declare it before any pass that samples the maps.

#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/hdri_sun.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::asset {
struct HdrImage;
}
namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
} // namespace engine::rhi

namespace engine::renderer {

struct HdriBakeSettings {
    float rotation_deg = 0.0F; // about +Y, counter-clockwise seen from above
    float intensity = 1.0F;    // multiplies the whole environment
    // Clamp the sun out of the maps; an analytic directional light
    // (sun_light()) carries its energy instead.
    bool remove_sun = true;

    bool operator==(const HdriBakeSettings&) const = default;
};

// The HDRI's sun as a directional light, in world space after the bake
// settings' rotation and intensity. `found` false: the panorama has no sun
// (overcast, interior); intensity is then 0.
struct HdriSunLight {
    bool      found = false;
    glm::vec3 direction_to_sun{0.0F, 1.0F, 0.0F};
    glm::vec3 color{1.0F};     // max component 1
    float     intensity = 0.0F; // colour * intensity = the clamped-off irradiance
};

class HdriEnvironment {
public:
    static constexpr std::uint32_t k_max_face = 1024;          // environment cube face cap
    static constexpr std::uint32_t k_min_face = 32;
    static constexpr std::uint32_t k_max_prefilter_face = 512; // prefiltered mip 0 cap
    static constexpr std::uint32_t k_sh_face = 32;             // SH9 source mip edge (max)
    static constexpr std::uint32_t k_prefilter_samples = 64;

    // Environment face edge for a panorama `width` texels wide: about one
    // panorama texel per cube texel at the equator, a power of two in
    // [k_min_face, k_max_face].
    [[nodiscard]] static std::uint32_t face_size_for(std::uint32_t width);

    [[nodiscard]] static std::expected<HdriEnvironment, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    HdriEnvironment();
    ~HdriEnvironment();
    HdriEnvironment(HdriEnvironment&&) noexcept;
    HdriEnvironment& operator=(HdriEnvironment&&) noexcept;
    HdriEnvironment(const HdriEnvironment&) = delete;
    HdriEnvironment& operator=(const HdriEnvironment&) = delete;

    // Uploads `image` (blocking, through `transfer`), analyses its sun and
    // (re)creates the maps; the next add_bake_to_graph bakes them. Maps from
    // an earlier image may still be sampled by frames in flight, so this
    // waits for the device to go idle first.
    [[nodiscard]] std::expected<void, core::Error> set_image(const asset::HdrImage& image,
                                                             const rhi::TransferContext& transfer);
    // Drops the image and the maps (waits for the device first).
    void clear();
    [[nodiscard]] bool has_image() const;

    // Declares the bake pass when the image or `settings` changed since the
    // last one; true when a bake was declared. Nothing without an image.
    [[nodiscard]] std::expected<bool, core::Error>
    add_bake_to_graph(rhi::RenderGraph& graph, const HdriBakeSettings& settings);
    // A bake has been declared for the current image (the maps are valid
    // from the frame that recorded it on).
    [[nodiscard]] bool baked() const;

    [[nodiscard]] IblViewSet views() const;             // invalid until baked
    [[nodiscard]] VkImageView environment_view() const; // full cube, every mip
    [[nodiscard]] const HdriSun& sun() const;
    [[nodiscard]] HdriSunLight sun_light(const HdriBakeSettings& settings) const;

    // Map geometry and images (tests read them back).
    [[nodiscard]] std::uint32_t face_size() const;
    [[nodiscard]] std::uint32_t prefilter_face_size() const;
    [[nodiscard]] VkImage environment_image() const;
    [[nodiscard]] VkImage prefiltered_image() const;
    [[nodiscard]] VkImage irradiance_image() const;
    [[nodiscard]] VkImage brdf_lut_image() const;
    // SH9 radiance coefficients of the last bake (rgb each; valid once the
    // bake's commands completed).
    [[nodiscard]] std::array<glm::vec3, 9> sh9() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace engine::renderer
