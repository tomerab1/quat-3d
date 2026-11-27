#pragma once

// WindFieldPass — the global wind field on the GPU (ROADMAP R13.1).
//
// One compute pass per frame (wind_field.slang) evaluates the weather's wind
// field (engine/weather/wind_field.hpp; lib/wind_common.slang is the GPU
// mirror of the CPU reference) into a wind_field_resolution² RGBA16F texture
// covering wind_field_extent_m around the camera: r = velocity x, g =
// velocity z, b = velocity y (m/s at the 10 m reference height), a = gust
// multiplier. The texture is snapped to whole texels so it does not swim as
// the camera moves; consumers (grass) sample it bilinearly by world XZ with
// WindFieldOutput::rect and scale it to their height with
// weather::wind_profile(). It is a transient graph image, so the graph orders
// the write before every reader.
//
// One instance per frame in flight (its descriptor buffer is rewritten each
// frame).

#include <array>
#include <cstdint>
#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/weather/wind_field.hpp"

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
} // namespace engine::rhi

namespace engine::renderer {

inline constexpr std::uint32_t wind_field_resolution = 128;
inline constexpr float         wind_field_texel_m = 2.0F; // 256 m around the camera
inline constexpr VkFormat      wind_field_format = VK_FORMAT_R16G16B16A16_SFLOAT;

// Mirrors WindFieldGpu in lib/wind_common.slang (push constants, 64 bytes).
struct WindFieldGpu {
    glm::vec4 flow{0.0F};   // xy unit heading (XZ), z mean speed, w gustiness
    glm::vec4 layers{0.0F}; // turbulence, gust offset, swirl offset, swirl phase
    glm::vec4 scales{0.0F}; // 1 / gust along, 1 / gust across, 1 / swirl cell, -
    glm::vec4 grid{0.0F};   // world XZ of texel (0, 0)'s corner, texel size m, -
};
static_assert(sizeof(WindFieldGpu) == 64, "must match lib/wind_common.slang WindFieldGpu");

// The push constants for `params` on a texture whose texel (0, 0) corner sits
// at world `corner_xz` with `texel_m` texels.
[[nodiscard]] WindFieldGpu pack_wind_field(const weather::WindFieldParams& params,
                                           glm::vec2 corner_xz, float texel_m);

// The texture's corner for a camera at `camera_xz`: centred, snapped to whole
// texels.
[[nodiscard]] glm::vec2 wind_field_corner(glm::vec2 camera_xz);

// One frame's wind texture in the graph.
struct WindFieldOutput {
    rhi::ResourceHandle texture; // wind_field_format, resolution²
    // (world X, world Z of the texture's corner, 1 / covered size m, 0) —
    // wind_texture_sample()'s `rect`.
    glm::vec4 rect{0.0F};
    // The mean wind's unit heading (XZ): constant over the field, for
    // phases of travelling waves (grass flutter) that must not follow the
    // local, turbulent direction.
    glm::vec2 heading{1.0F, 0.0F};

    [[nodiscard]] bool valid() const { return texture.valid(); }
};

class WindFieldPass {
public:
    [[nodiscard]] static std::expected<WindFieldPass, core::Error>
    create(const rhi::Device& device, rhi::GpuAllocator& allocator, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    WindFieldPass() = default;
    WindFieldPass(WindFieldPass&&) noexcept = default;
    WindFieldPass& operator=(WindFieldPass&&) noexcept = default;
    WindFieldPass(const WindFieldPass&) = delete;
    WindFieldPass& operator=(const WindFieldPass&) = delete;

    [[nodiscard]] bool valid() const { return pipeline_.handle() != VK_NULL_HANDLE; }

    // Declares the wind texture and the compute pass that fills it with
    // `params` around `camera_xz`. The descriptor buffer is owned by the pass
    // and rewritten by the next call, so the graph must have executed (the
    // frame slot's fence waited) before reuse.
    [[nodiscard]] std::expected<WindFieldOutput, core::Error>
    add_to_graph(rhi::RenderGraph& graph, const weather::WindFieldParams& params,
                 glm::vec2 camera_xz);

private:
    const rhi::Device* device_ = nullptr;    // non-owning
    rhi::GpuAllocator* allocator_ = nullptr; // non-owning

    rhi::DescriptorBufferFunctions db_fns_{};
    rhi::DescriptorSetLayout       layout_;
    rhi::ManagedPipeline           pipeline_;
    std::uint32_t                  out_slot_ = 0; // g_wind's reflected binding
    rhi::DescriptorBuffer          frame_descriptor_;
};

} // namespace engine::renderer
