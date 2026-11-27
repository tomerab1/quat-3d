#pragma once

// Shared scene pieces for the R12 shadow / AO tests: axis-aligned boxes and
// quads as uploaded meshes (no glTF), a camera, and read-back helpers for
// the Hi-Z pyramid and depth buffer.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace quat::test::occlusion {

// A box spanning [lo, hi] with outward normals (24 vertices, 36 indices).
[[nodiscard]] inline std::expected<engine::asset::MeshAsset, engine::core::Error>
upload_box(GpuContext& g, const glm::vec3& lo, const glm::vec3& hi) {
    struct Face {
        glm::vec3 n, u, v;
    };
    const std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    const glm::vec3 c = (lo + hi) * 0.5F;
    const glm::vec3 h = (hi - lo) * 0.5F;
    std::vector<engine::asset::Vertex> verts;
    std::vector<std::uint32_t>         indices;
    for (const Face& f : faces) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        const std::array<glm::vec2, 4> corners{{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
        for (const glm::vec2& k : corners) {
            engine::asset::Vertex v;
            v.position = c + h * (f.n + f.u * k.x + f.v * k.y);
            v.normal = f.n;
            v.uv = (k + 1.0F) * 0.5F;
            v.tangent = glm::vec4(f.u, 1.0F);
            verts.push_back(v);
        }
        for (std::uint32_t i : {0U, 1U, 2U, 0U, 2U, 3U}) indices.push_back(base + i);
    }
    return upload_mesh(g, verts, indices);
}

// A grey, fully rough dielectric.
[[nodiscard]] inline std::expected<engine::asset::MaterialAsset, engine::core::Error>
upload_grey(GpuContext& g, float albedo = 0.7F) {
    engine::asset::PbrMaterialParams params;
    params.base_color_factor = {albedo, albedo, albedo, 1.0F};
    params.metallic_factor = 0.0F;
    params.roughness_factor = 1.0F;
    return engine::asset::upload_material(params, engine::asset::MaterialTextures{}, g.allocator,
                                          g.transfer);
}

// Perspective camera (Vulkan Y flip, like camera_system).
struct Camera {
    glm::mat4 view{1.0F};
    glm::mat4 proj{1.0F};
    glm::mat4 view_proj{1.0F};
    glm::vec3 position{0.0F};
};

[[nodiscard]] inline Camera look_at(const glm::vec3& eye, const glm::vec3& target,
                                    float fov_deg = 60.0F, float aspect = 1.0F,
                                    float near_z = 0.1F, float far_z = 200.0F) {
    Camera c;
    c.position = eye;
    c.view = glm::lookAt(eye, target, glm::vec3(0.0F, 1.0F, 0.0F));
    c.proj = glm::perspective(glm::radians(fov_deg), aspect, near_z, far_z);
    c.proj[1][1] *= -1.0F;
    c.view_proj = c.proj * c.view;
    return c;
}

// Pixel of a world point under `view_proj` for an `extent` target.
[[nodiscard]] inline glm::uvec2 project(const glm::mat4& view_proj, const glm::vec3& p,
                                        VkExtent2D extent) {
    const glm::vec4 clip = view_proj * glm::vec4(p, 1.0F);
    const glm::vec2 ndc = glm::vec2(clip) / clip.w;
    const glm::vec2 uv = ndc * 0.5F + 0.5F;
    return {static_cast<std::uint32_t>(uv.x * static_cast<float>(extent.width)),
            static_cast<std::uint32_t>(uv.y * static_cast<float>(extent.height))};
}

// Levels of a read-back RG32F pyramid plus the D32 depth it was built from.
struct PyramidReadback {
    std::vector<VkExtent2D>              extents;
    std::vector<std::vector<glm::vec2>>  levels;
    std::vector<float>                   depth;
};

// Appends a copy of every pyramid level and of `depth` to `graph`, runs it
// and returns the texels.
[[nodiscard]] inline std::expected<PyramidReadback, engine::core::Error>
read_pyramid(GpuContext& g, engine::rhi::RenderGraph& graph, engine::rhi::ResourceHandle pyramid,
             engine::rhi::ResourceHandle depth, VkExtent2D extent, std::uint32_t mips) {
    PyramidReadback out;
    std::vector<VkDeviceSize> offsets;
    VkDeviceSize              total = 0;
    VkExtent2D                level = extent;
    for (std::uint32_t m = 0; m < mips; ++m) {
        out.extents.push_back(level);
        offsets.push_back(total);
        total += static_cast<VkDeviceSize>(level.width) * level.height * 8;
        level = {std::max(level.width / 2, 1U), std::max(level.height / 2, 1U)};
    }
    const VkDeviceSize depth_offset = total;
    total += static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
    auto readback = host_readback_buffer(g.allocator, total);
    if (!readback) return std::unexpected(readback.error());
    const VkBuffer dst = readback->handle();
    const std::vector<VkExtent2D> extents = out.extents;
    (void)graph.add_pass("test_pyramid_readback", engine::rhi::PassType::transfer)
        .reads(pyramid, engine::rhi::ResourceUsage::transfer_src)
        .reads(depth, engine::rhi::ResourceUsage::transfer_src)
        .execute([=](engine::rhi::PassContext& ctx) {
            std::vector<VkBufferImageCopy> regions;
            for (std::uint32_t m = 0; m < extents.size(); ++m) {
                VkBufferImageCopy r{};
                r.bufferOffset = offsets[m];
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
                r.imageExtent = {extents[m].width, extents[m].height, 1};
                regions.push_back(r);
            }
            vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(pyramid).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                                   static_cast<std::uint32_t>(regions.size()), regions.data());
            VkBufferImageCopy d{};
            d.bufferOffset = depth_offset;
            d.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            d.imageExtent = {extent.width, extent.height, 1};
            vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(depth).image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &d);
        });
    if (auto c = graph.compile(); !c) return std::unexpected(c.error());
    auto done = submit_and_wait(g.device, [&](VkCommandBuffer cmd) {
        graph.execute(cmd);
        transfer_to_host_barrier(cmd);
    });
    if (!done) return std::unexpected(done.error());
    if (auto inv = readback->invalidate_mapped(); !inv) return std::unexpected(inv.error());
    const auto* bytes = static_cast<const std::uint8_t*>(readback->mapped());
    for (std::uint32_t m = 0; m < mips; ++m) {
        std::vector<glm::vec2> texels(static_cast<std::size_t>(out.extents[m].width) *
                                      out.extents[m].height);
        std::memcpy(texels.data(), bytes + offsets[m], texels.size() * sizeof(glm::vec2));
        out.levels.push_back(std::move(texels));
    }
    out.depth.resize(static_cast<std::size_t>(extent.width) * extent.height);
    std::memcpy(out.depth.data(), bytes + depth_offset, out.depth.size() * sizeof(float));
    return out;
}

} // namespace quat::test::occlusion
