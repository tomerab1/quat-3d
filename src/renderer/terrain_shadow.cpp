// TerrainPass as a sun-shadow caster (R12): the depth-only pipeline over the
// same chunk geometry, recorded by ShadowPass inside each cascade.

#include <array>
#include <cstddef>
#include <string>
#include <utility>

#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace engine::renderer {

namespace {

// Mirrors PushConstants in terrain_shadow.slang (64 bytes).
struct TerrainShadowPush {
    VkDeviceAddress cascades = 0;
    std::uint32_t   cascade = 0;
    std::uint32_t   pad = 0;
    glm::vec4       chunk{0.0F};  // origin xz, chunk size m, skirt depth m
    glm::vec4       region{0.0F}; // uv offset, uv scale
    glm::vec4       grid{0.0F};   // grid_n, tile origin y
};
static_assert(sizeof(TerrainShadowPush) == 64, "must match terrain_shadow.slang PushConstants");
static_assert(offsetof(TerrainShadowPush, chunk) == 16, "must match terrain_shadow.slang");

constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// The heightmap set the GBuffer pipeline's layout was built with; the shadow
// shader must declare the same slots (checked against its reflection).
constexpr std::array<rhi::BindingRequest, 2> kShadowResources{{
    {"g_heightmap", VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE},
    {"g_sampler", VK_DESCRIPTOR_TYPE_SAMPLER},
}};

} // namespace

std::expected<void, core::Error>
TerrainPass::create_shadow_pipeline(rhi::PipelineCache& cache,
                                    const std::string& cooked_shader_dir) {
    auto shader = cache.load(cooked_shader_dir + "/terrain_shadow.spv");
    if (!shader) return std::unexpected(shader.error());
    auto bindings = rhi::reflected_bindings((*shader)->reflection, kShadowResources, kStages);
    if (!bindings) {
        return std::unexpected(core::Error{"terrain shadow: " + bindings.error().message});
    }
    if ((*bindings)[0].binding != 0 || (*bindings)[1].binding != 1) {
        return std::unexpected(core::Error{
            "terrain shadow: terrain_shadow.slang must bind the heightmap and sampler at "
            "slots 0 and 1, like the terrain set layout"});
    }
    const VkPushConstantRange pc{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(TerrainShadowPush)};
    const VkDescriptorSetLayout set_layout = layout_.handle();
    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.depth_format = shadow_map_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.cull_mode = VK_CULL_MODE_NONE; // top faces + skirts
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    shadow_pipeline_ = std::move(*pipeline);
    return {};
}

ShadowCasterFn TerrainPass::shadow_casters(const glm::mat4& cull_view_proj,
                                           const glm::vec3& camera_pos,
                                           const glm::vec3& anchor) {
    collect_chunks(cull_view_proj, camera_pos, anchor, shadow_draws_);
    if (shadow_draws_.empty()) return {};
    const float origin_y = anchor.y;
    return [this, origin_y](VkCommandBuffer cmd, std::uint32_t cascade,
                            VkDeviceAddress cascades) {
        record_shadow(cmd, cascade, cascades, origin_y);
    };
}

void TerrainPass::record_shadow(VkCommandBuffer cmd, std::uint32_t cascade,
                                VkDeviceAddress cascades, float origin_y) const {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_.handle());
    for (const TileDraws& draws : shadow_draws_) {
        draws.tile->descriptor.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    shadow_pipeline_.layout(), 0);
        for (const ChunkDraw& draw : draws.chunks) {
            TerrainShadowPush push{};
            push.cascades = cascades;
            push.cascade = cascade;
            push.chunk = draw.chunk;
            push.region = draw.region;
            push.grid = glm::vec4(static_cast<float>(chunk_quads), origin_y, 0.0F, 0.0F);
            vkCmdPushConstants(cmd, shadow_pipeline_.layout(), VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdBindIndexBuffer(cmd, lods_[draw.lod].buffer.handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, lods_[draw.lod].count, 1, 0, 0, 0);
        }
    }
}

} // namespace engine::renderer
