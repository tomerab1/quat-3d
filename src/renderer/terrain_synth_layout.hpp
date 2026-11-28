#pragma once

// The C++ side of terrain_synth.slang's interface (TerrainSynthPass): the
// uniform block, push constants, entry points and resources, mirrored field
// for field. Private to the renderer.

#include <array>
#include <cstdint>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/rhi/shader_reflection.hpp"
#include "engine/terrain/generator.hpp"

namespace engine::renderer::terrain_synth {

// Mirrors SynthParams in terrain_synth.slang (std140, 128 bytes).
struct SynthUniforms {
    glm::ivec4 tile{0};  // world tile x, z; resolution; octaves
    glm::vec4  noise{0}; // noise texel scale, base frequency, lacunarity, gain
    glm::vec4  shape{0}; // warp strength, height (m)
    glm::vec4  step0{0}; // dt, rain * dt, pipe, cell area
    glm::vec4  step1{0}; // 1 / area, cell, 1 / cell, cell * sqrt(2)
    glm::vec4  step2{0}; // advect, Ks * dt, Kd * dt, 1 - Ke * dt
    glm::vec4  step3{0}; // thermal / 8, 1 / max depth, capacity, min tilt
    glm::vec4  step4{0}; // talus
};
static_assert(sizeof(SynthUniforms) == 128, "must match terrain_synth.slang SynthParams");

// Mirrors Push in terrain_synth.slang.
struct SynthPush {
    std::uint32_t cur = 0;     // sediment half
    std::uint32_t relaxed = 0; // synth_finalize reads g_relax[src..]
    std::uint32_t res = 0;     // relax_*: grid written / read, texels per side
    std::uint32_t src_res = 0;
    std::uint32_t src = 0;     // relax_*: offsets into g_relax (floats)
    std::uint32_t dst = 0;
    std::uint32_t orig = 0;
    float         limit_axis = 0.0F;
    float         limit_diagonal = 0.0F;
};
static_assert(sizeof(SynthPush) == 36, "must match terrain_synth.slang Push");

enum Kernel : std::uint8_t {
    k_init, k_flux, k_water, k_apply, k_transport,
    k_relax_load, k_relax_restrict, k_relax_sweep, k_relax_prolong,
    k_finalize, k_count
};
constexpr std::array<const char*, k_count> kEntries{
    "synth_init",   "erode_flux",     "erode_water", "erode_apply",   "erode_transport",
    "relax_load",   "relax_restrict", "relax_sweep", "relax_prolong", "synth_finalize"};

// terrain_synth.slang's resources, by name; their binding slots come from the
// shader's reflection (State::slots, indexed by Resource).
enum Resource : std::uint8_t {
    r_params, r_perm, r_height, r_water, r_sediment, r_flux, r_flow, r_out, r_relax, r_count
};
constexpr std::array<rhi::BindingRequest, r_count> kResources{{
    {"g_params", VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER},
    {"g_perm", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_height", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_water", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_sediment", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_flux", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_flow", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_out", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
    {"g_relax", VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
}};

[[nodiscard]] inline SynthUniforms make_uniforms(const terrain::TerrainParams& p, std::uint32_t res) {
    SynthUniforms u;
    u.tile = glm::ivec4(p.world_tile.x, p.world_tile.y, static_cast<std::int32_t>(res),
                        p.octaves);
    u.noise = glm::vec4(terrain::noise_texel_scale(p), p.base_frequency, p.lacunarity, p.gain);
    u.shape = glm::vec4(p.warp_strength, p.height_m, 0.0F, 0.0F);
    const float cell = p.tile_size_m / static_cast<float>(res - 1); // Heightmap::metres_per_texel
    const terrain::GridErosionStep k = terrain::grid_erosion_step(p.grid, cell);
    u.step0 = glm::vec4(k.dt, k.rain_dt, k.pipe, k.area);
    u.step1 = glm::vec4(k.inv_area, k.cell, k.inv_cell, k.diagonal);
    u.step2 = glm::vec4(k.advect, k.erode_dt, k.deposit_dt, k.keep_water);
    u.step3 = glm::vec4(k.thermal, k.inv_max_depth, k.capacity, k.min_tilt);
    u.step4 = glm::vec4(k.talus, 0.0F, 0.0F, 0.0F);
    return u;
}

} // namespace engine::renderer::terrain_synth
