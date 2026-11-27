#include "engine/renderer/terrain_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/terrain/generator.hpp"

namespace engine::renderer {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

// Mirrors PushConstants in terrain.slang (208 bytes).
struct TerrainPushConstants {
    glm::mat4 view_proj{1.0F};
    glm::vec4 chunk{0.0F};  // origin xz, chunk size m, skirt depth m
    glm::vec4 region{0.0F}; // uv offset, uv scale, metres per texel
    glm::vec4 shade{0.0F};  // grid_n, heightmap texel uv, snowline m, rock slope
    glm::vec4 misc{0.0F};   // x = origin y, yzw unused
    glm::mat4 prev_view_proj{1.0F}; // velocity: last frame, unjittered
    glm::vec4 motion{0.0F};         // xy = this frame's jitter (NDC)
};
static_assert(sizeof(TerrainPushConstants) == 208, "must match terrain.slang PushConstants");

constexpr VkShaderStageFlags kStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

// Vertex-id helpers matching decode_vertex() in terrain.slang.
[[nodiscard]] std::uint32_t grid_id(std::uint32_t x, std::uint32_t z) {
    return z * (chunk_quads + 1) + x;
}

// Ring slot of a perimeter grid vertex (corners belong to the following side).
[[nodiscard]] std::uint32_t ring_slot(std::uint32_t x, std::uint32_t z) {
    const std::uint32_t n = chunk_quads;
    if (z == 0 && x < n) return x;            // -z edge, left to right
    if (x == n && z < n) return n + z;        // +x edge
    if (z == n && x > 0) return 2 * n + (n - x); // +z edge, right to left
    return 3 * n + (n - z);                   // -x edge (and the (0,0) wrap)
}

[[nodiscard]] std::uint32_t skirt_id(std::uint32_t x, std::uint32_t z) {
    const std::uint32_t grid_count = (chunk_quads + 1) * (chunk_quads + 1);
    return grid_count + (ring_slot(x, z) % (4 * chunk_quads));
}

// Index buffer for one geomipmap level: the decimated grid plus the skirt.
[[nodiscard]] std::vector<std::uint32_t> build_lod_indices(std::uint32_t step) {
    const std::uint32_t n = chunk_quads;
    std::vector<std::uint32_t> idx;
    idx.reserve(static_cast<std::size_t>(n / step) * (n / step) * 6 + (n / step) * 4 * 6);

    for (std::uint32_t z = 0; z < n; z += step) {
        for (std::uint32_t x = 0; x < n; x += step) {
            const std::uint32_t i00 = grid_id(x, z);
            const std::uint32_t i10 = grid_id(x + step, z);
            const std::uint32_t i01 = grid_id(x, z + step);
            const std::uint32_t i11 = grid_id(x + step, z + step);
            idx.insert(idx.end(), {i00, i01, i10, i10, i01, i11});
        }
    }

    // Skirt quads around the perimeter (pipeline culls nothing, so winding is
    // free; the quads connect each edge vertex to its dropped twin).
    const auto edge_quad = [&](std::uint32_t ax, std::uint32_t az, std::uint32_t bx,
                               std::uint32_t bz) {
        const std::uint32_t ga = grid_id(ax, az);
        const std::uint32_t gb = grid_id(bx, bz);
        const std::uint32_t sa = skirt_id(ax, az);
        const std::uint32_t sb = skirt_id(bx, bz);
        idx.insert(idx.end(), {ga, gb, sb, ga, sb, sa});
    };
    for (std::uint32_t t = 0; t < n; t += step) {
        edge_quad(t, 0, t + step, 0);         // -z
        edge_quad(n, t, n, t + step);         // +x
        edge_quad(t + step, n, t, n);         // +z
        edge_quad(0, t + step, 0, t);         // -x
    }
    return idx;
}

// Gribb–Hartmann frustum planes from a view-projection matrix (Vulkan depth
// [0, 1]); planes point inward, w is the distance term.
[[nodiscard]] std::array<glm::vec4, 6> frustum_planes(const glm::mat4& vp) {
    const glm::mat4 m = glm::transpose(vp);
    return {m[3] + m[0], m[3] - m[0], m[3] + m[1], m[3] - m[1], m[2], m[3] - m[2]};
}

[[nodiscard]] bool aabb_visible(const std::array<glm::vec4, 6>& planes, const glm::vec3& mn,
                                const glm::vec3& mx) {
    for (const glm::vec4& p : planes) {
        // Positive vertex: the AABB corner furthest along the plane normal.
        const glm::vec3 v(p.x >= 0.0F ? mx.x : mn.x, p.y >= 0.0F ? mx.y : mn.y,
                          p.z >= 0.0F ? mx.z : mn.z);
        if (glm::dot(glm::vec3(p), v) + p.w < 0.0F) return false;
    }
    return true;
}

} // namespace

std::expected<TerrainPass, core::Error>
TerrainPass::create(const rhi::Device& device, rhi::GpuAllocator& allocator,
                    rhi::PipelineCache& cache, const rhi::TransferContext& transfer,
                    const std::string& cooked_shader_dir) {
    TerrainPass out;
    out.device_ = &device;
    out.allocator_ = &allocator;

    out.db_fns_ = rhi::DescriptorBufferFunctions::load(device.handle());
    if (!out.db_fns_.valid()) return fail("terrain pass: descriptor buffer functions unavailable");

    using rhi::DescriptorBinding;
    const std::array<DescriptorBinding, 2> bindings{{
        {0, 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, kStages},
        {0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, kStages},
    }};
    auto layout = rhi::DescriptorSetLayout::create(device, out.db_fns_, bindings);
    if (!layout) return std::unexpected(layout.error());
    out.layout_ = std::move(*layout);

    auto shader = cache.load(cooked_shader_dir + "/terrain.spv");
    if (!shader) return std::unexpected(shader.error());

    const VkPushConstantRange pc{kStages, 0, sizeof(TerrainPushConstants)};
    const VkDescriptorSetLayout set_layout = out.layout_.handle();

    rhi::GraphicsPipeline::CreateInfo info{};
    info.vertex = *shader;
    info.fragment = *shader;
    info.color_formats = gbuffer_color_formats;
    info.depth_format = gbuffer_depth_format;
    info.depth_compare_op = VK_COMPARE_OP_LESS_OR_EQUAL; // standard [0,1] depth
    info.cull_mode = VK_CULL_MODE_NONE;                  // top faces + skirts
    info.set_layouts = {&set_layout, 1};
    info.push_constants = {&pc, 1};
    auto pipeline = cache.create_graphics_pipeline(info);
    if (!pipeline) return std::unexpected(pipeline.error());
    out.pipeline_ = std::move(*pipeline);

    auto sampler = rhi::create_sampler(device.handle(), rhi::SamplerAddress::clamp_to_edge);
    if (!sampler) return std::unexpected(sampler.error());
    out.sampler_ = std::move(*sampler);

    out.lods_.reserve(lod_count);
    for (std::uint32_t lod = 0; lod < lod_count; ++lod) {
        const std::vector<std::uint32_t> indices = build_lod_indices(1U << lod);
        auto buf = rhi::upload_device_buffer(allocator, transfer, indices.data(),
                                             indices.size() * sizeof(std::uint32_t),
                                             VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (!buf) return std::unexpected(buf.error());
        out.lods_.push_back({std::move(*buf), static_cast<std::uint32_t>(indices.size())});
    }

    if (auto shadow = out.create_shadow_pipeline(cache, cooked_shader_dir); !shadow) {
        return std::unexpected(shadow.error());
    }

    // GPU synthesis is optional: without it tiles generate on the CPU.
    if (auto gpu = GpuTileSource::create(device, allocator, cache, cooked_shader_dir); gpu) {
        out.gpu_tiles_ = std::move(*gpu);
    } else {
        std::fprintf(stderr,
                     "[terrain] GPU synthesis unavailable (%s); tiles generate on the CPU\n",
                     gpu.error().message.c_str());
    }
    return out;
}

std::expected<void, core::Error> TerrainPass::set_tile(const glm::ivec2& coord,
                                                       const terrain::Heightmap& map,
                                                       const rhi::TransferContext& transfer) {
    if (!map.valid()) return fail("terrain pass: invalid heightmap");

    Tile tile;
    std::optional<rhi::GpuImage> synthesised;
    if (map.gpu_tile != 0 && gpu_tiles_) synthesised = gpu_tiles_->take_image(map.gpu_tile);
    if (synthesised) {
        tile.image = std::move(*synthesised); // the GPU already wrote these heights
    } else {
        auto image = rhi::upload_device_image(
            *allocator_, transfer, map.heights.data(), map.heights.size() * sizeof(float),
            {map.resolution, map.resolution}, VK_FORMAT_R32_SFLOAT);
        if (!image) return std::unexpected(image.error());
        tile.image = std::move(*image);
    }
    auto view = rhi::create_image_view(device_->handle(), tile.image.handle(),
                                       VK_FORMAT_R32_SFLOAT);
    if (!view) return std::unexpected(view.error());
    tile.view = std::move(*view);
    tile.resolution = map.resolution;
    tile.tile_size_m = map.tile_size_m;
    tile.min_height = map.min_height;
    tile.max_height = map.max_height;

    auto db = rhi::DescriptorBuffer::create(*device_, *allocator_, db_fns_, layout_);
    if (!db) return std::unexpected(db.error());
    db->write_sampled_image(0, tile.view.handle(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    db->write_sampler(1, sampler_.handle());
    tile.descriptor = std::move(*db);

    if (auto it = tiles_.find(coord); it != tiles_.end()) {
        retired_.emplace_back(3, std::move(it->second)); // outlive in-flight frames
        tiles_.erase(it);
    }
    tiles_.emplace(coord, std::move(tile));
    return {};
}

void TerrainPass::tick() {
    // Destroy retired tiles once every frame that could have referenced them
    // has left the GPU.
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (--it->first <= 0) {
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
    if (gpu_tiles_) gpu_tiles_->tick();
}

void TerrainPass::remove_tile(const glm::ivec2& coord) {
    if (auto it = tiles_.find(coord); it != tiles_.end()) {
        retired_.emplace_back(3, std::move(it->second));
        tiles_.erase(it);
    }
}

std::vector<TerrainHeightTile> TerrainPass::height_tiles(const glm::vec2& centre, float radius,
                                                        const glm::vec3& anchor,
                                                        std::size_t max_tiles) const {
    std::vector<std::pair<float, TerrainHeightTile>> found;
    for (const auto& [coord, tile] : tiles_) {
        const float tsize = tile.tile_size_m;
        // Tile (0,0) is centred on the anchor (as add_to_graph places it).
        const glm::vec2 origin(anchor.x + (static_cast<float>(coord.x) - 0.5F) * tsize,
                               anchor.z + (static_cast<float>(coord.y) - 0.5F) * tsize);
        const glm::vec2 nearest = glm::clamp(centre, origin, origin + tsize);
        const float dist = glm::length(nearest - centre);
        if (dist > radius) continue;
        TerrainHeightTile t;
        t.view = tile.view.handle();
        t.origin_xz = origin;
        t.size_m = tsize;
        t.base_y = anchor.y;
        t.min_height = tile.min_height;
        t.max_height = tile.max_height;
        t.resolution = tile.resolution;
        found.emplace_back(dist, t);
    }
    std::ranges::sort(found, {}, &std::pair<float, TerrainHeightTile>::first);
    std::vector<TerrainHeightTile> out;
    for (const auto& [dist, t] : found) {
        if (out.size() >= max_tiles) break;
        out.push_back(t);
    }
    return out;
}

void TerrainPass::collect_chunks(const glm::mat4& cull_view_proj, const glm::vec3& camera_pos,
                                 const glm::vec3& anchor, std::vector<TileDraws>& out) const {
    const auto planes = frustum_planes(cull_view_proj);
    out.clear();
    for (const auto& [coord, tile] : tiles_) {
        // Tile (0,0) is centred on the anchor; neighbours continue the grid.
        const float tsize = tile.tile_size_m;
        const glm::vec3 origin(anchor.x + (static_cast<float>(coord.x) - 0.5F) * tsize, anchor.y,
                               anchor.z + (static_cast<float>(coord.y) - 0.5F) * tsize);

        const std::uint32_t chunks = glm::max((tile.resolution - 1) / chunk_quads, 1U);
        const float chunk_size = tsize / static_cast<float>(chunks);
        const float skirt = glm::max((tile.max_height - tile.min_height) * 0.05F, 1.0F);

        TileDraws draws;
        draws.tile = &tile;
        for (std::uint32_t cz = 0; cz < chunks; ++cz) {
            for (std::uint32_t cx = 0; cx < chunks; ++cx) {
                const glm::vec2 corner(origin.x + static_cast<float>(cx) * chunk_size,
                                       origin.z + static_cast<float>(cz) * chunk_size);
                const glm::vec3 mn(corner.x, origin.y + tile.min_height - skirt, corner.y);
                const glm::vec3 mx(corner.x + chunk_size, origin.y + tile.max_height,
                                   corner.y + chunk_size);
                if (!aabb_visible(planes, mn, mx)) continue;

                const glm::vec2 centre = corner + chunk_size * 0.5F;
                const float dist = glm::length(centre - glm::vec2(camera_pos.x, camera_pos.z));
                const float ratio = glm::max(dist / (chunk_size * 2.0F), 1.0F);
                const auto lod = glm::min(static_cast<std::uint32_t>(std::log2(ratio)),
                                          lod_count - 1);

                ChunkDraw draw;
                draw.chunk = glm::vec4(corner, chunk_size, skirt);
                const float uv_scale = 1.0F / static_cast<float>(chunks);
                draw.region = glm::vec4(static_cast<float>(cx) * uv_scale,
                                        static_cast<float>(cz) * uv_scale, uv_scale,
                                        tsize / static_cast<float>(tile.resolution - 1));
                draw.lod = lod;
                draws.chunks.push_back(draw);
            }
        }
        if (!draws.chunks.empty()) out.push_back(std::move(draws));
    }
}

void TerrainPass::add_to_graph(rhi::RenderGraph& graph, const renderer::GBufferTargets& gbuffer,
                               VkExtent2D extent, const glm::mat4& view_proj,
                               const glm::vec3& camera_pos, const glm::vec3& anchor,
                               float snowline_m) {
    // GPU tile synthesis records its compute work into this frame (it is
    // independent of the draws below and of the GBuffer).
    if (gpu_tiles_) gpu_tiles_->add_to_graph(graph);
    if (tiles_.empty()) return;

    collect_chunks(view_proj, camera_pos, anchor, frame_draws_);
    if (frame_draws_.empty()) return;

    const GBufferTargets t = gbuffer;
    const glm::mat4 vp = view_proj;
    const float origin_y = anchor.y;
    graph.add_pass("terrain", rhi::PassType::graphics)
        .writes(t.albedo, rhi::ResourceUsage::color_attachment)
        .writes(t.normal, rhi::ResourceUsage::color_attachment)
        .writes(t.material, rhi::ResourceUsage::color_attachment)
        .writes(t.clearcoat, rhi::ResourceUsage::color_attachment)
        .writes(t.velocity, rhi::ResourceUsage::color_attachment)
        .writes(t.depth, rhi::ResourceUsage::depth_attachment)
        .execute([this, t, extent, vp, origin_y, snowline_m](rhi::PassContext& ctx) {
            const VkCommandBuffer cmd = ctx.cmd();
            const auto color_attachment = [](VkImageView view) {
                VkRenderingAttachmentInfo ai{};
                ai.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ai.imageView = view;
                ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ai.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // over the mesh pass
                ai.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                return ai;
            };
            const std::array<VkRenderingAttachmentInfo, gbuffer_color_formats.size()> colors{
                color_attachment(ctx.resolve(t.albedo).view),
                color_attachment(ctx.resolve(t.normal).view),
                color_attachment(ctx.resolve(t.material).view),
                color_attachment(ctx.resolve(t.clearcoat).view),
                color_attachment(ctx.resolve(t.velocity).view)};

            VkRenderingAttachmentInfo depth{};
            depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            depth.imageView = ctx.resolve(t.depth).view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, extent};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
            rendering.pColorAttachments = colors.data();
            rendering.pDepthAttachment = &depth;
            vkCmdBeginRendering(cmd, &rendering);

            VkViewport viewport{};
            viewport.width = static_cast<float>(extent.width);
            viewport.height = static_cast<float>(extent.height);
            viewport.maxDepth = 1.0F;
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            const VkRect2D scissor{{0, 0}, extent};
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_.handle());

            for (const TileDraws& draws : frame_draws_) {
                draws.tile->descriptor.bind(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            pipeline_.layout(), 0);
                const float texel_uv = 1.0F / static_cast<float>(draws.tile->resolution);
                for (const ChunkDraw& draw : draws.chunks) {
                    TerrainPushConstants push{};
                    push.view_proj = vp;
                    push.chunk = draw.chunk;
                    push.region = draw.region;
                    push.shade = glm::vec4(static_cast<float>(chunk_quads), texel_uv,
                                           snowline_m, 0.72F);
                    push.misc = glm::vec4(origin_y, 0.0F, 0.0F, 0.0F);
                    push.prev_view_proj = t.motion.prev_view_proj;
                    push.motion = glm::vec4(t.motion.jitter_ndc, 0.0F, 0.0F);
                    vkCmdPushConstants(cmd, pipeline_.layout(), kStages, 0, sizeof(push), &push);
                    vkCmdBindIndexBuffer(cmd, lods_[draw.lod].buffer.handle(), 0,
                                         VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, lods_[draw.lod].count, 1, 0, 0, 0);
                }
            }
            vkCmdEndRendering(cmd);
        });
}

} // namespace engine::renderer
