// FrameRenderer's R21 lighting environment: the HDRI panorama (loading,
// baking, its sun as the scene's directional light), the choice between it
// and the dynamic sky probe each frame, and the screen-space reflections.
// Kept out of frame_graph.cpp so the graph build only calls in.
//
// A8 settings: `environment.source` hdri lights and draws the scene with the
// loaded HDRI (rotation, intensity; `hdri_sun` turns its sun into the
// shadowed directional light) and falls back to the procedural / atmosphere
// sky while none is loaded; `ssr` drives SsrPass; `ibl.multiscatter` and
// `ibl.horizon_occlusion` go to the lighting pass.

#include "app/frame_renderer.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "engine/asset/hdr_decode.hpp"
#include "engine/asset/texture_limits.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/remote/world_commands.hpp" // confine_to_root
#include "engine/rhi/device.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

namespace {

// A transient upload context on the graphics queue (HDRI uploads are rare
// and blocking, like scene loads).
template <typename Fn>
[[nodiscard]] std::expected<void, engine::core::Error>
with_upload(const engine::rhi::Device& device, Fn&& fn) {
    const VkDevice vk = device.handle();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = device.queue_families().graphics;
    if (vkCreateCommandPool(vk, &info, nullptr, &pool) != VK_SUCCESS) {
        return std::unexpected(engine::core::Error{"hdri: vkCreateCommandPool failed"});
    }
    const engine::rhi::TransferContext transfer{vk, pool, device.graphics_queue(),
                                                device.max_sampler_anisotropy(),
                                                device.texture_caps()};
    auto r = std::forward<Fn>(fn)(transfer);
    vkDestroyCommandPool(vk, pool, nullptr);
    return r;
}

} // namespace

void FrameRenderer::create_reflections(engine::rhi::PipelineCache& pipeline_cache,
                                       const std::string& shader_dir) {
    if (auto env = engine::renderer::HdriEnvironment::create(*device_, *allocator_, pipeline_cache,
                                                             shader_dir);
        env) {
        hdri_.emplace(std::move(*env));
    } else {
        std::fprintf(stderr, "[warn] HDRI environment unavailable: %s\n",
                     env.error().message.c_str());
    }
    if (auto ssr = engine::renderer::SsrPass::create(*device_, *allocator_, pipeline_cache,
                                                     shader_dir, k_frames_in_flight);
        ssr) {
        ssr_.emplace(std::move(*ssr));
    } else {
        std::fprintf(stderr, "[warn] screen-space reflections unavailable: %s\n",
                     ssr.error().message.c_str());
    }
}

std::expected<void, engine::core::Error>
FrameRenderer::set_hdri(const std::string& relative, const std::filesystem::path& project_root) {
    hdri_status_ = {};
    hdri_status_.path = relative;
    if (relative.empty()) {
        if (hdri_) hdri_->clear();
        return {};
    }
    const auto failed = [&](std::string why) -> std::expected<void, engine::core::Error> {
        if (hdri_) hdri_->clear();
        hdri_status_.error = why;
        std::fprintf(stderr, "[warn] hdri '%s': %s\n", relative.c_str(), why.c_str());
        return std::unexpected(engine::core::Error{std::move(why)});
    };
    if (!hdri_) return failed("the HDRI environment is unavailable (renderer setup failed)");
    auto path = engine::remote::confine_to_root(project_root, relative);
    if (!path) return failed(path.error().message);
    const engine::asset::TextureLimits limits =
        engine::asset::clamp_to_device(engine::asset::TextureLimits{}, device_->texture_caps());
    auto image = engine::asset::load_radiance_hdr(*path, limits);
    if (!image) return failed(image.error().message);
    auto uploaded = with_upload(*device_, [&](const engine::rhi::TransferContext& transfer) {
        return hdri_->set_image(*image, transfer);
    });
    if (!uploaded) return failed(uploaded.error().message);
    hdri_status_.loaded = true;
    hdri_status_.width = image->width;
    hdri_status_.height = image->height;
    hdri_status_.sun_found = hdri_->sun().found;
    std::fprintf(stderr, "[info] hdri '%s' loaded (%ux%u, %s)\n", relative.c_str(), image->width,
                 image->height, hdri_status_.sun_found ? "sun found" : "no sun");
    // Reflections and the temporal history saw the old lighting.
    temporal_.reset();
    return {};
}

bool FrameRenderer::hdri_active() const {
    return hdri_ && hdri_->has_image() &&
           engine::renderer::sky_source(settings_.environment) ==
               engine::renderer::SkySource::hdri;
}

engine::renderer::HdriBakeSettings FrameRenderer::hdri_bake_settings() const {
    const engine::renderer::EnvironmentSettings& e = settings_.environment;
    engine::renderer::HdriBakeSettings b;
    b.rotation_deg = engine::renderer::EnvironmentSettings::k_rotation_deg.clamp(e.hdri_rotation_deg);
    b.intensity = engine::renderer::EnvironmentSettings::k_intensity.clamp(e.hdri_intensity);
    b.remove_sun = e.hdri_sun;
    return b;
}

void FrameRenderer::apply_hdri_sun(engine::renderer::DirectionalLightParams& light) const {
    if (!hdri_active() || !settings_.environment.hdri_sun) return;
    const engine::renderer::HdriSunLight sun = hdri_->sun_light(hdri_bake_settings());
    // No sun in the panorama (overcast, interior): no directional light.
    light.direction = glm::vec4(sun.direction_to_sun, 0.0F);
    light.color = glm::vec4(sun.color, sun.found ? sun.intensity : 0.0F);
}

FrameRenderer::EnvironmentMaps FrameRenderer::add_environment(engine::rhi::RenderGraph& graph,
                                                              const glm::vec3& sun_to) {
    EnvironmentMaps maps;
    if (hdri_active()) {
        if (auto r = hdri_->add_bake_to_graph(graph, hdri_bake_settings()); !r) {
            std::fprintf(stderr, "[warn] hdri bake: %s\n", r.error().message.c_str());
        }
        maps.ibl = hdri_->views();
        maps.background = hdri_->environment_view();
        return maps;
    }
    // The sky probe advances one step per frame and flips when a cycle
    // completes (declared before every consumer: the graph keeps
    // declaration order for independent passes).
    if (dynamic_ibl_) {
        engine::renderer::IblCycleParams cycle{};
        cycle.sun_to = sun_to;
        cycle.skyview = atmosphere_ ? atmosphere_->skyview_view() : VK_NULL_HANDLE;
        cycle.transmittance = atmosphere_ ? atmosphere_->transmittance_view() : VK_NULL_HANDLE;
        cycle.clouds = current_clouds();
        if (auto r = dynamic_ibl_->add_step_to_graph(graph, cycle); !r) {
            std::fprintf(stderr, "[warn] ibl step: %s\n", r.error().message.c_str());
        }
        maps.ibl = dynamic_ibl_->views();
    }
    return maps;
}

engine::rhi::ResourceHandle
FrameRenderer::add_reflections(engine::rhi::RenderGraph& graph, const FrameScene& fs,
                               const engine::renderer::GBufferTargets& gbuffer,
                               const engine::renderer::HiZTargets& hiz, const glm::vec3& camera) {
    if (!ssr_) return {};
    engine::renderer::SsrInputs in;
    in.depth = gbuffer.depth;
    in.normal = gbuffer.normal;
    in.material = gbuffer.material;
    in.velocity = gbuffer.velocity;
    in.hiz = hiz;
    in.view_proj = fs.jittered_vp;
    in.camera_pos = camera;
    in.extent = fs.render_extent;
    in.frame_index = fs.frame_number;
    in.reset = fs.reset_history;
    auto r = ssr_->add_to_graph(graph, frame_, in, settings_.ssr);
    if (!r) {
        std::fprintf(stderr, "[warn] ssr: %s\n", r.error().message.c_str());
        return {};
    }
    return *r;
}

void FrameRenderer::add_reflection_color(engine::rhi::RenderGraph& graph,
                                         engine::rhi::ResourceHandle hdr) {
    if (ssr_) ssr_->add_color_copy(graph, hdr);
}

engine::renderer::LightingTuning
FrameRenderer::lighting_tuning(engine::rhi::ResourceHandle reflections,
                               VkImageView background) const {
    engine::renderer::LightingTuning t;
    t.ibl_intensity = settings_.ibl.intensity;
    t.multiscatter = settings_.ibl.multiscatter;
    t.horizon_occlusion = settings_.ibl.horizon_occlusion;
    t.reflections = reflections;
    t.environment = background;
    return t;
}

} // namespace quat::app
