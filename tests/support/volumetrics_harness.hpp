#pragma once

// Shared GPU harness for the volumetrics tests (R13.3 / R13.4): renders the
// aerial perspective, the volumetric clouds and the atmospherics composite
// headless for a camera over an empty (sky-only) or flat-depth scene, frame
// after frame with the temporal histories alive, and reads the results back.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

#include "engine/renderer/aerial_perspective_pass.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/atmospherics_pass.hpp"
#include "engine/renderer/cloud_noise_pass.hpp"
#include "engine/renderer/cloud_reference.hpp"
#include "engine/renderer/fog_pass.hpp"
#include "engine/renderer/volumetric_cloud_pass.hpp"
#include "engine/rhi/render_graph.hpp"
#include "support/gpu_context.hpp"
#include "support/render_readback.hpp"

namespace quat::test {

struct VolumetricsScene {
    glm::vec3 camera{0.0F, 2.0F, 0.0F};
    glm::vec3 target{0.0F, 2.6F, -10.0F}; // looking north, a little up
    float     fov_deg = 70.0F;
    glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 0.5F, -0.6F));
    engine::renderer::CloudSettings clouds{};
    VkExtent2D extent{320, 180};
    float      scene_depth = 1.0F; // depth-buffer value everywhere (1 = sky)
    bool       atmosphere = true;  // aerial perspective + clouds
    // Fog (R13.3): runs FogPass when `fog` is set.
    bool                                       fog = false;
    engine::renderer::FogMedia                 media{};
    std::vector<engine::renderer::FogVolumeGpu> volumes;
    glm::vec4  background{0.0F, 0.0F, 0.0F, 1.0F}; // HDR clear colour
};

// One frame's readbacks: the accumulated clouds (cloud extent) and the
// composite over a constant background (render extent).
struct VolumetricsFrame {
    HostImage clouds;    // RGBA16F: rgb luminance (LUT units), a transmittance
    HostImage composite; // RGBA16F
};

class VolumetricsHarness {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<VolumetricsHarness>, engine::core::Error>
    create(GpuContext& g) {
        auto h = std::make_unique<VolumetricsHarness>(g);
        auto atmos = engine::renderer::AtmospherePass::create(g.device, g.allocator, g.cache,
                                                              g.shader_dir);
        if (!atmos) return std::unexpected(atmos.error());
        h->atmosphere_ = std::move(*atmos);
        auto noise = engine::renderer::CloudNoisePass::create(g.device, g.allocator, g.cache,
                                                              g.shader_dir);
        if (!noise) return std::unexpected(noise.error());
        h->noise_ = std::move(*noise);
        h->history_ = std::make_shared<engine::renderer::CloudHistory>(g.device, g.allocator, 1);
        auto ap = engine::renderer::AerialPerspectivePass::create(g.device, g.allocator, g.cache,
                                                                  g.shader_dir);
        if (!ap) return std::unexpected(ap.error());
        h->ap_ = std::move(*ap);
        auto clouds = engine::renderer::VolumetricCloudPass::create(g.device, g.allocator, g.cache,
                                                                    g.shader_dir, h->history_);
        if (!clouds) return std::unexpected(clouds.error());
        h->clouds_ = std::move(*clouds);
        auto comp = engine::renderer::AtmosphericsPass::create(g.device, g.allocator, g.cache,
                                                               g.shader_dir);
        if (!comp) return std::unexpected(comp.error());
        h->composite_ = std::move(*comp);
        h->fog_history_ = std::make_shared<engine::renderer::FogHistory>(g.device, g.allocator);
        auto fog = engine::renderer::FogPass::create(g.device, g.allocator, g.cache, g.shader_dir,
                                                     h->fog_history_);
        if (!fog) return std::unexpected(fog.error());
        h->fog_ = std::move(*fog);
        return h;
    }

    explicit VolumetricsHarness(GpuContext& g) : g_(&g) {}

    // Renders frame `frame` of `scene` (the clouds' clock at `time_s`).
    [[nodiscard]] std::expected<VolumetricsFrame, engine::core::Error>
    render(const VolumetricsScene& scene, std::uint64_t frame, float time_s) {
        namespace r = engine::renderer;
        GpuContext& g = *g_;
        if (auto s = atmosphere_->ensure_skyview(scene.sun_to); !s) return std::unexpected(s.error());
        const float aspect = static_cast<float>(scene.extent.width) /
                             static_cast<float>(scene.extent.height);
        glm::mat4 proj = glm::perspective(glm::radians(scene.fov_deg), aspect, 0.1F, 5000.0F);
        proj[1][1] *= -1.0F; // Vulkan clip space, as the engine's cameras
        const glm::mat4 view = glm::lookAt(scene.camera, scene.target, glm::vec3(0, 1, 0));
        const glm::mat4 vp = proj * view;
        r::CloudSettings c = scene.clouds;
        c.time_s = time_s;
        const r::CloudLayerGpu layer =
            r::make_cloud_layer(c, false, scene.camera, r::CloudLights{.sun_to = scene.sun_to});

        engine::rhi::TransientImagePool pool(g.device, g.allocator);
        engine::rhi::RenderGraph graph(pool);
        const engine::rhi::ResourceHandle depth = graph.create_transient_image(
            "test_depth", VK_FORMAT_D32_SFLOAT, scene.extent,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        const engine::rhi::ResourceHandle hdr = graph.create_transient_image(
            "test_hdr", VK_FORMAT_R16G16B16A16_SFLOAT, scene.extent,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        const float clear_depth = scene.scene_depth;
        const glm::vec4 bg = scene.background;
        (void)graph.add_pass("test_clear", engine::rhi::PassType::transfer)
            .writes(depth, engine::rhi::ResourceUsage::transfer_dst)
            .writes(hdr, engine::rhi::ResourceUsage::transfer_dst)
            .execute([depth, hdr, clear_depth, bg](engine::rhi::PassContext& ctx) {
                const VkClearDepthStencilValue dv{clear_depth, 0};
                const VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                vkCmdClearDepthStencilImage(ctx.cmd(), ctx.resolve(depth).image,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &dr);
                const VkClearColorValue cv{{bg.r, bg.g, bg.b, bg.a}};
                const VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(ctx.cmd(), ctx.resolve(hdr).image,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &cr);
            });
        r::AtmosphericsInputs ai;
        ai.hdr = hdr;
        ai.depth = depth;
        ai.extent = scene.extent;
        ai.inv_view_proj = glm::inverse(vp);
        ai.camera = scene.camera;
        if (scene.atmosphere) {
            r::AerialPerspectiveInputs api;
            api.inv_view_proj = glm::inverse(vp);
            api.sun_to = scene.sun_to;
            api.transmittance = atmosphere_->transmittance_view();
            api.multiscatter = atmosphere_->multiscatter_view();
            auto apv = ap_.add_to_graph(graph, api);
            if (!apv) return std::unexpected(apv.error());
            r::VolumetricCloudInputs ci;
            ci.layer = layer;
            ci.inv_view_proj = glm::inverse(vp);
            ci.prev_view_proj = has_prev_ ? prev_vp_ : vp;
            ci.camera = scene.camera;
            ci.drift_m = has_prev_
                             ? (glm::vec2(layer.offset.x, layer.offset.y) - prev_anchor_) * -1000.0F
                             : glm::vec2(0.0F);
            ci.frame_index = frame;
            ci.reset = !has_prev_;
            ci.march_steps = scene.clouds.march_steps;
            ci.depth = depth;
            ci.render_extent = scene.extent;
            ci.aerial_perspective = *apv;
            ci.transmittance = atmosphere_->transmittance_view();
            ci.skyview = atmosphere_->skyview_view();
            auto cimg = clouds_.add_to_graph(graph, ci, *noise_);
            if (!cimg) return std::unexpected(cimg.error());
            ai.aerial_perspective = *apv;
            ai.clouds = *cimg;
        }
        if (scene.fog) {
            r::FogInputs fi;
            fi.media = scene.media;
            fi.volumes = scene.volumes;
            fi.inv_view_proj = glm::inverse(vp);
            fi.prev_view_proj = has_prev_ ? prev_vp_ : vp;
            fi.camera = scene.camera;
            fi.prev_camera = scene.camera;
            fi.frame_index = frame;
            fi.reset = !has_prev_;
            auto fv = fog_.add_to_graph(graph, fi);
            if (!fv) return std::unexpected(fv.error());
            ai.fog = *fv;
            ai.media = scene.media;
            ai.height_fog = scene.media.extinction > 0.0F;
        }
        prev_vp_ = vp;
        prev_anchor_ = glm::vec2(layer.offset.x, layer.offset.y);
        has_prev_ = true;
        if (auto a = composite_.add_to_graph(graph, ai); !a) return std::unexpected(a.error());
        if (!ai.clouds.valid()) {
            auto comp = render_to_host(g, graph, hdr, scene.extent, 8);
            if (!comp) return std::unexpected(comp.error());
            VolumetricsFrame out;
            out.composite = std::move(*comp);
            return out;
        }
        const r::CloudImage cloud_image = ai.clouds;

        // Read the clouds (cloud extent) in a pass of its own, then the composite.
        const VkExtent2D ce = r::cloud_extent(scene.extent);
        auto rb = host_readback_buffer(g.allocator, static_cast<VkDeviceSize>(ce.width) * ce.height * 8);
        if (!rb) return std::unexpected(rb.error());
        const VkBuffer dst = rb->handle();
        const engine::rhi::ResourceHandle cloud_color = cloud_image.color;
        (void)graph.add_pass("test_cloud_readback", engine::rhi::PassType::transfer)
            .reads(cloud_color, engine::rhi::ResourceUsage::transfer_src)
            .execute([=](engine::rhi::PassContext& ctx) {
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {ce.width, ce.height, 1};
                vkCmdCopyImageToBuffer(ctx.cmd(), ctx.resolve(cloud_color).image,
                                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
            });
        auto comp = render_to_host(g, graph, hdr, scene.extent, 8);
        if (!comp) return std::unexpected(comp.error());
        if (auto inv = rb->invalidate_mapped(); !inv) return std::unexpected(inv.error());
        VolumetricsFrame out;
        out.composite = std::move(*comp);
        out.clouds.extent = ce;
        out.clouds.bytes_per_pixel = 8;
        out.clouds.bytes.resize(static_cast<std::size_t>(ce.width) * ce.height * 8);
        std::memcpy(out.clouds.bytes.data(), rb->mapped(), out.clouds.bytes.size());
        return out;
    }

    void reset_history() { has_prev_ = false; }

private:
    GpuContext*                                    g_;
    std::optional<engine::renderer::AtmospherePass> atmosphere_;
    std::optional<engine::renderer::CloudNoisePass> noise_;
    std::shared_ptr<engine::renderer::CloudHistory> history_;
    engine::renderer::AerialPerspectivePass         ap_;
    engine::renderer::VolumetricCloudPass           clouds_;
    engine::renderer::AtmosphericsPass              composite_;
    std::shared_ptr<engine::renderer::FogHistory>   fog_history_;
    engine::renderer::FogPass                       fog_;
    glm::mat4 prev_vp_{1.0F};
    glm::vec2 prev_anchor_{0.0F};
    bool      has_prev_ = false;
};

// Writes an RGBA16F image as a tonemapped (Reinhard, sRGB-ish gamma) PPM —
// a debugging aid enabled with QUAT_TEST_DUMP_DIR.
inline void dump_ppm(const HostImage& img, const std::string& name, float scale) {
    const char* dir = std::getenv("QUAT_TEST_DUMP_DIR");
    if (dir == nullptr) return;
    std::ofstream f(std::string(dir) + "/" + name + ".ppm", std::ios::binary);
    f << "P6\n" << img.extent.width << " " << img.extent.height << "\n255\n";
    for (std::uint32_t y = 0; y < img.extent.height; ++y) {
        for (std::uint32_t x = 0; x < img.extent.width; ++x) {
            const glm::vec3 c = glm::vec3(img.rgba16f(x, y)) * scale;
            for (int k = 0; k < 3; ++k) {
                const float t = c[k] / (1.0F + c[k]);
                f.put(static_cast<char>(static_cast<unsigned char>(
                    glm::clamp(std::pow(t, 1.0F / 2.2F), 0.0F, 1.0F) * 255.0F + 0.5F)));
            }
        }
    }
}

} // namespace quat::test
