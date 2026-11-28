// FrameRenderer, per frame: gathers the ticked scene into draw lists and builds
// the render graph. Pass order (graph declaration order):
//   sky-view LUT update, HDRI bake or dynamic IBL step, wind field, GBuffer,
//   terrain, grass, background velocity, Hi-Z, cascaded shadows (GPU fit +
//   cascades), GTAO, screen-space reflections, light culling, cloud shadow
//   map, lighting, transparent, particles, reflection colour copy (all at
//   the render extent), upscaler (render -> display extent), depth of field,
//   motion blur (R20, frame_post.cpp), bloom, exposure, tonemap / grade,
//   present copy, editor UI; GPU skinning is recorded before the graph
//   executes.

#include "app/frame_renderer.hpp"
#include "app/grass_runtime.hpp"
#include "app/particle_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/core/profile.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/material_override.hpp"
#include "engine/renderer/weather_surfaces.hpp"
#include "engine/scene/weather_frame.hpp"

namespace quat::app {
namespace {

#ifdef ENGINE_EDITOR
// (Re)creates the slot's offscreen viewport image at `extent`. Safe to replace
// here: the slot's fence was waited before its frame is rebuilt. The image is
// in the LDR (UNORM) format: TAA copies the tonemapped, already sRGB-encoded
// bytes into it as into the swapchain, and the UI samples those bytes
// undecoded and blends them in gamma space (issue #92), so the Viewport panel
// shows exactly the bytes the game view presents.
bool ensure_viewport_target(DeferredFrame& frame, const engine::rhi::Device& device,
                            engine::rhi::GpuAllocator& allocator, VkFormat format,
                            VkExtent2D extent) {
    if (frame.viewport_image.handle() != VK_NULL_HANDLE &&
        frame.viewport_extent.width == extent.width &&
        frame.viewport_extent.height == extent.height) {
        return true;
    }
    // TRANSFER_SRC: remote screenshots copy the scene view out of it.
    auto image = allocator.create_image(format, extent,
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_SAMPLED_BIT);
    if (!image) return false;
    auto view = engine::rhi::create_image_view(device.handle(), image->handle(), format);
    if (!view) return false;
    frame.viewport_image = std::move(*image);
    frame.viewport_view = std::move(*view);
    frame.viewport_extent = extent;
    return true;
}
#endif

// Fetch (or create) this frame slot's skinning buffers for `entity`. Returns
// nullptr if allocation fails (the caller then renders the bind pose).
SkinnedEntityGpu* ensure_skin_gpu(DeferredFrame& frame, entt::entity entity,
                                  const engine::asset::MeshAsset& mesh, VkDevice vk_device,
                                  engine::rhi::GpuAllocator& allocator, std::uint32_t joint_count) {
    auto it = frame.skin_gpu.find(entity);
    if (it != frame.skin_gpu.end() && it->second.vertex_count == mesh.vertex_count &&
        it->second.joint_count == joint_count) {
        return &it->second;
    }

    const VkDeviceSize joints_size = static_cast<VkDeviceSize>(joint_count) * sizeof(glm::mat4);
    const VkDeviceSize verts_size =
        static_cast<VkDeviceSize>(mesh.vertex_count) * sizeof(engine::asset::Vertex);

    const auto host_joints = [&] {
        return allocator.create_buffer(
            joints_size,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VMA_MEMORY_USAGE_AUTO,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT);
    };
    auto joints = host_joints();
    auto prev_joints = host_joints();
    if (!joints || !prev_joints) return nullptr;
    auto verts = allocator.create_buffer(
        verts_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!verts) return nullptr;
    // Velocity: positions skinned with last frame's palette (float4 each).
    auto prev_positions = allocator.create_buffer(
        static_cast<VkDeviceSize>(mesh.vertex_count) * sizeof(glm::vec4),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!prev_positions) return nullptr;

    SkinnedEntityGpu g;
    g.joints = std::move(*joints);
    g.prev_joints = std::move(*prev_joints);
    g.skinned_verts = std::move(*verts);
    g.prev_positions = std::move(*prev_positions);
    g.joints_addr = engine::rhi::buffer_device_address(vk_device, g.joints);
    g.prev_joints_addr = engine::rhi::buffer_device_address(vk_device, g.prev_joints);
    g.skinned_addr = engine::rhi::buffer_device_address(vk_device, g.skinned_verts);
    g.prev_positions_addr = engine::rhi::buffer_device_address(vk_device, g.prev_positions);
    g.vertex_count = mesh.vertex_count;
    g.joint_count = joint_count;
    frame.skin_gpu[entity] = std::move(g);
    return &frame.skin_gpu[entity];
}

} // namespace

engine::renderer::CloudSettings FrameRenderer::current_clouds() const {
    return engine::scene::frame_clouds(settings_.clouds, cloud_time_s_,
                                       has_weather_ ? &weather_ : nullptr, cloud_drift_km_,
                                       camera_pos_);
}

void FrameRenderer::advance_time(float dt) {
    cloud_time_s_ += dt;
    material_time_s_ += static_cast<double>(dt);
    for (DeferredFrame& frame : frames_) {
        frame.mesh.set_material_time(static_cast<float>(material_time_s_));
    }
    if (has_weather_) {
        cloud_drift_km_ += engine::scene::cloud_wind(weather_, settings_.clouds) * (dt * 0.001F);
    }
}

engine::renderer::CsmSettings FrameRenderer::current_csm() const {
    return settings_.shadows.sanitized();
}

engine::renderer::GtaoSettings FrameRenderer::current_gtao() const {
    return settings_.ao.sanitized();
}

engine::renderer::ContactShadowSettings FrameRenderer::current_contact_shadows() const {
    return settings_.contact_shadows.sanitized();
}

FrameScene FrameRenderer::gather(engine::scene::Scene& scene,
                                 const engine::scene::CameraMatrices& cam,
                                 VkExtent2D draw_extent, std::uint64_t frame_number) {
    QUAT_ZONE_N("scene_gather");
    const VkDevice vk_device = device_->handle();
    engine::rhi::GpuAllocator& allocator = *allocator_;
    DeferredFrame& fr = frames_[frame_];
    FrameScene out;

    // R11: render extent, jitter, view motion, camera-cut flag (frame_temporal.cpp).
    begin_view(out, cam, draw_extent, frame_number);

    engine::renderer::DirectionalLightParams& light = out.light;
    const auto light_view = scene.registry().view<const engine::scene::DirectionalLight>();
    for (const entt::entity e : light_view) {
        const auto& dl = light_view.get<const engine::scene::DirectionalLight>(e);
        light.direction = glm::vec4(-glm::normalize(dl.direction), 0.0F);
        light.color = glm::vec4(dl.color, dl.intensity);
        break;
    }
    apply_hdri_sun(light); // R21: the HDRI's own sun, when selected

    // Gather point lights from the ECS (world position from Transform); the
    // light-cull compute pass bins them into view clusters on the GPU.
    for (auto [e, t, pl] :
         scene.registry()
             .view<const engine::scene::Transform, const engine::scene::PointLight>()
             .each()) {
        out.point_lights.push_back(engine::renderer::make_point_light(
            glm::vec3(t.world[3]), pl.radius, pl.color, pl.intensity));
    }

    // Build the draw list, collecting per-skinned-entity skinning work. A
    // skinned entity uploads its joint matrices into this frame slot's buffer
    // and gets a SkinDispatch; its draw points at the skinned output buffer.
    std::vector<engine::renderer::DrawItem>& draws = out.draws;
    std::vector<engine::renderer::DrawItem>& transparent_draws = out.transparent_draws;
    for (auto [e, t, mr] :
         scene.registry().view<engine::scene::Transform, engine::scene::MeshRenderer>().each()) {
        engine::renderer::DrawItem item;
        item.mesh = mr.mesh.valid() ? &*mr.mesh : nullptr;
        item.material = material_overrides_.drawn(scene.registry(), e, mr);
        item.model = t.world;
        const auto key = static_cast<std::uint64_t>(entt::to_integral(e));
        item.prev_model = temporal_.motion().advance(key, t.world);

        const auto* sm = scene.registry().try_get<engine::scene::SkinnedMesh>(e);
        if (item.mesh != nullptr && item.mesh->skinned() && sm != nullptr &&
            !sm->joint_matrices.empty()) {
            SkinnedEntityGpu* g = ensure_skin_gpu(
                fr, e, *item.mesh, vk_device, allocator,
                static_cast<std::uint32_t>(sm->joint_matrices.size()));
            if (g != nullptr) {
                const VkDeviceSize joint_bytes = sm->joint_matrices.size() * sizeof(glm::mat4);
                const std::span<const glm::mat4> prev_palette =
                    temporal_.motion().advance_palette(key, sm->joint_matrices);
                std::memcpy(g->joints.mapped(), sm->joint_matrices.data(), joint_bytes);
                std::memcpy(g->prev_joints.mapped(), prev_palette.data(), joint_bytes);
                // Sequential-write host memory may be non-coherent (S1.A15).
                for (engine::rhi::GpuBuffer* b : {&g->joints, &g->prev_joints}) {
                    if (auto flushed = b->flush(0, joint_bytes); !flushed) {
                        std::fprintf(stderr, "[warn] joint matrix flush: %s\n",
                                     flushed.error().message.c_str());
                    }
                }
                engine::renderer::SkinDispatch d;
                d.vertices_in =
                    engine::rhi::buffer_device_address(vk_device, item.mesh->vertex_buffer);
                d.vertices_in_size = static_cast<VkDeviceSize>(item.mesh->vertex_count) *
                                     sizeof(engine::asset::Vertex);
                d.skin = engine::rhi::buffer_device_address(vk_device, item.mesh->skin_buffer);
                d.skin_size = static_cast<VkDeviceSize>(item.mesh->vertex_count) *
                              sizeof(engine::asset::SkinVertex);
                d.joints = g->joints_addr;
                d.joints_size = static_cast<VkDeviceSize>(g->joint_count) * sizeof(glm::mat4);
                d.vertices_out = g->skinned_addr;
                d.vertices_out_size = static_cast<VkDeviceSize>(g->vertex_count) *
                                      sizeof(engine::asset::Vertex);
                d.vertex_count = item.mesh->vertex_count;
                d.prev_joints = g->prev_joints_addr;
                d.prev_positions_out = g->prev_positions_addr;
                d.prev_positions_out_size =
                    static_cast<VkDeviceSize>(g->vertex_count) * sizeof(glm::vec4);
                out.dispatches.push_back(d);
                item.skinned_vertices = g->skinned_verts.handle();
                item.prev_positions = g->prev_positions_addr;
                // Skinned vertices are already in skin space (glTF ignores the
                // mesh node transform), so draw them with an identity model.
                item.model = glm::mat4(1.0F);
                item.prev_model = glm::mat4(1.0F);
            }
        }
        // Route alpha-blended + transmissive (glass) materials to the forward pass.
        const bool is_transparent =
            item.material != nullptr &&
            (item.material->params.flags &
             (engine::asset::material_blend | engine::asset::material_transmission)) != 0;
        (is_transparent ? transparent_draws : draws).push_back(item);
    }

    // R9.2: blended / transmissive InstancedMesh parts, one item per instance.
    engine::scene::append_instanced_transparent_items(scene.registry(), cam.view_proj,
                                                      transparent_draws);
    // Sort transparent draws back-to-front (farthest from the camera first).
    std::sort(transparent_draws.begin(), transparent_draws.end(),
              [&](const engine::renderer::DrawItem& a, const engine::renderer::DrawItem& b) {
                  const float da = glm::distance(glm::vec3(a.model[3]), cam.position);
                  const float db = glm::distance(glm::vec3(b.model[3]), cam.position);
                  return da > db;
              });

    // Shadow casters: everything drawn. The shadow pass routes them by
    // material — opaque and alpha-tested (MASK) casters into the depth
    // cascades, transmissive (glass) and alpha-blended ones into the
    // translucency map, so they cast tinted / partial shadows (R12.11).
    out.shadow_draws = draws;
    out.shadow_draws.insert(out.shadow_draws.end(), transparent_draws.begin(),
                            transparent_draws.end());

    // Sun shadows: cascades are fitted to the unjittered camera (on the GPU,
    // from this frame's depth bounds); casters are culled against the volume
    // every possible cascade fits in.
    out.csm_view = {cam.view, cam.projection, glm::vec3(light.direction)};
    out.shadow_cull_view_proj = engine::renderer::caster_cull_view_proj(
        out.csm_view, current_csm(), engine::renderer::shadow_map_size);
    out.frame_number = frame_number;

    // R13.1: the weather this frame renders.
    out.has_weather = engine::scene::frame_weather(scene.registry(), out.weather);
    weather_ = out.weather;
    has_weather_ = out.has_weather;
    camera_pos_ = cam.position;
    return out;
}

bool FrameRenderer::record(const FrameInputs& in) {
    QUAT_ZONE_N("record_frame");
    const engine::rhi::Device& device = *device_;
    engine::rhi::GpuAllocator& allocator = *allocator_;
    engine::rhi::Swapchain& swapchain = *swapchain_;
    const FrameScene& fs = *in.scene;
    const engine::scene::CameraMatrices& cam = *in.cam;
    const VkExtent2D draw_extent = in.draw_extent;
    const VkExtent2D present_extent = swapchain.extent();
    const VkCommandBuffer cmd = command_buffers_[frame_];
    const std::uint32_t image_index = image_index_;
    DeferredFrame& fr = frames_[frame_];
    draw_extent_ = draw_extent;
    // R11: the scene renders at render_extent; the upscaler resolves to
    // draw_extent, where post-processing and the UI run.
    const VkExtent2D render_extent = fs.render_extent;
    apply_texture_lod_bias(fr, render_extent, draw_extent);
    const auto atmos_skyview = [&]() {
        return atmosphere_ ? atmosphere_->skyview_view() : VK_NULL_HANDLE;
    };
    const auto atmos_transmittance = [&]() {
        return atmosphere_ ? atmosphere_->transmittance_view() : VK_NULL_HANDLE;
    };

    engine::rhi::RenderGraph graph(fr.pool);

    // Sky + environment upkeep, declared FIRST so their trailing barriers
    // precede every consumer this frame (the graph keeps declaration order
    // for independent passes). The sky-view LUT recomputes in-graph when
    // the sun moved (no pipeline stall on sun drags); the environment maps
    // are the HDRI's (baked when its inputs change) or the dynamic IBL
    // probe's (one step per frame, flipping when a cycle completes).
    const glm::vec3 sun_to = glm::vec3(fs.light.direction);
    if (atmosphere_) {
        if (auto r = atmosphere_->add_skyview_update_to_graph(graph, sun_to); !r) {
            std::fprintf(stderr, "[warn] sky-view update: %s\n", r.error().message.c_str());
        }
    }
    const EnvironmentMaps environment = add_environment(graph, sun_to);
    const engine::renderer::IblViewSet ibl_views = environment.ibl;

    // R13.1: with weather, the global wind field around the camera (grass
    // samples it), advanced by simulated time so pausing freezes the wind
    // with the world. Without weather nothing runs: grass keeps its R9.3 sway.
    wind_clock_.advance(engine::weather::sanitized(fs.weather).wind_speed, in.sim_dt);
    // Last frame's field is evaluated too, so grass motion vectors (R11)
    // carry the gusts sweeping through, not only the flutter.
    engine::renderer::WindFieldOutput wind{};
    if (fs.has_weather && fr.wind.valid()) {
        const engine::weather::WindFieldParams wind_params =
            engine::weather::wind_field_params(fs.weather, wind_clock_);
        if (auto w = fr.wind.add_to_graph(graph, wind_params,
                                          glm::vec2(cam.position.x, cam.position.z),
                                          prev_wind_params_)) {
            wind = *w;
        } else {
            std::fprintf(stderr, "[warn] wind field: %s\n", w.error().message.c_str());
        }
        prev_wind_params_ = wind_params;
    } else {
        prev_wind_params_.reset(); // weather (re)appearing starts without field motion
    }

    // R9.1: draws sharing mesh + material collapse into instanced draws;
    // R9.2: InstancedMesh placements join them, one draw per (mesh,
    // material), culled per instance (shadow casters against the cascades'
    // cull volume). R11: the opaque placements record their previous
    // transforms for the velocity target.
    const engine::scene::FrameDrawLists batches = engine::scene::build_frame_draw_lists(
        *in.registry, fs.draws, fs.shadow_draws, cam.view_proj, fs.shadow_cull_view_proj,
        &temporal_.motion());
    const engine::renderer::InstancedDrawList& shadow_batches = batches.shadow;
    const engine::renderer::InstancedDrawList& opaque_batches = batches.opaque;
    const engine::renderer::RenderSettings& settings = settings_;
    auto gbuffer = fr.mesh.add_to_graph(graph, render_extent, fs.jittered_vp, opaque_batches,
                                        fs.motion);
    if (!gbuffer) {
        std::fprintf(stderr, "[fatal] gbuffer pass: %s\n", gbuffer.error().message.c_str());
        return false;
    }
    if (in.terrain != nullptr) {
        in.terrain->add_to_graph(graph, *gbuffer, render_extent, fs.jittered_vp, cam.position,
                                 in.terrain_anchor, in.terrain_snowline);
    }
    if (in.grass != nullptr) {
        in.grass->add_to_graph(graph, frame_, *in.registry, *gbuffer, render_extent,
                               fs.jittered_vp, cam, in.terrain, in.terrain_anchor, in.sim_dt,
                               wind);
    }
    // R11: camera-only velocity where no geometry was drawn (after every
    // opaque writer).
    velocity_.add_background_to_graph(graph, *gbuffer, render_extent, cam.view_proj);
    // R12: Hi-Z pyramid of the final depth (after terrain / grass), then the
    // sun's cascades fitted to its depth bounds and rendered (shadows.enabled
    // off: no shadow passes, the lighting is unshadowed).
    const engine::renderer::CsmSettings csm = current_csm();
    // A5 debug.stats + the editor's Stats panel: draws after batching (the
    // shadow casters only when the cascades are rendered).
    draw_count_ = engine::scene::frame_draw_count(batches, fs.transparent_draws.size()) -
                  (csm.enabled ? 0 : shadow_batches.draws.size());
    plot_frame_counters(engine::scene::frame_draw_stats(batches, fs.transparent_draws, csm.enabled));
    engine::renderer::HiZTargets hiz{};
    if (fr.hiz.valid()) {
        if (auto h = fr.hiz.add_to_graph(graph, gbuffer->depth, render_extent)) {
            hiz = *h;
        } else {
            std::fprintf(stderr, "[warn] hi-z pass: %s\n", h.error().message.c_str());
        }
    }
    std::vector<engine::renderer::ShadowCasterFn> extra_casters;
    if (in.terrain != nullptr && csm.enabled) {
        if (auto fn = in.terrain->shadow_casters(fs.shadow_cull_view_proj, cam.position,
                                                 in.terrain_anchor)) {
            extra_casters.push_back(std::move(fn));
        }
    }
    auto shadows = fr.shadow.add_to_graph(graph, fs.csm_view, csm, hiz.valid() ? &hiz : nullptr,
                                          shadow_batches, extra_casters);
    if (!shadows) {
        std::fprintf(stderr, "[fatal] shadow pass: %s\n", shadows.error().message.c_str());
        return false;
    }
    engine::renderer::LightingOcclusion occlusion;
    occlusion.shadows = *shadows;
    occlusion.csm = csm;
    occlusion.gtao = current_gtao();
    if (hiz.valid() && fr.gtao.valid()) {
        if (auto ao = fr.gtao.add_to_graph(graph, hiz, gbuffer->normal, render_extent, cam.view,
                                           cam.projection, occlusion.gtao,
                                           static_cast<std::uint32_t>(fs.frame_number))) {
            occlusion.ao = *ao;
        } else {
            std::fprintf(stderr, "[warn] gtao pass: %s\n", ao.error().message.c_str());
        }
    }
    // R21: screen-space reflections over the environment (Hi-Z traced).
    const engine::rhi::ResourceHandle reflections =
        hiz.valid() ? add_reflections(graph, fs, *gbuffer, hiz, cam.position)
                    : engine::rhi::ResourceHandle{};
    occlusion.contact = current_contact_shadows();
    occlusion.projection = cam.projection;
    occlusion.frame_index = static_cast<std::uint32_t>(fs.frame_number);
    // Clustered light culling (R10.1). A camera the clusters cannot
    // represent (non-perspective / no finite far plane) renders unlit by
    // point lights rather than stopping the frame loop.
    engine::renderer::ClusteredLights clustered{};
    if (auto c = fr.light_cull.add_to_graph(graph, cam.view, cam.projection, fs.point_lights)) {
        clustered = *c;
    } else {
        std::fprintf(stderr, "[warn] light cull: %s\n", c.error().message.c_str());
    }
    // R13.1: with weather, the cloud shadow map (one compute pass; the
    // lighting pass samples it). Needs the atmosphere's cloud layer.
    const engine::renderer::CloudSettings clouds = current_clouds();
    engine::renderer::CloudShadowMap cloud_shadow{};
    if (fs.has_weather && atmosphere_ && clouds.enabled && clouds.shadows &&
        fr.cloud_shadow.valid()) {
        if (auto m = fr.cloud_shadow.add_to_graph(graph, sun_to, cam.position, clouds)) {
            cloud_shadow = *m;
        } else {
            std::fprintf(stderr, "[warn] cloud shadow map: %s\n", m.error().message.c_str());
        }
    }
    // R21 terms (frame_environment.cpp) plus the R13.1 weather surfaces and
    // cloud shadows.
    engine::renderer::LightingTuning tuning =
        lighting_tuning(reflections, environment.background);
    tuning.weather = engine::renderer::WeatherSurfaces::from(
        fs.weather,
        in.terrain != nullptr ? in.terrain_snowline : engine::renderer::k_no_snowline_m);
    tuning.cloud_shadow = cloud_shadow;
    auto hdr = fr.lighting.add_to_graph(graph, *gbuffer, render_extent, fs.light,
                                        fs.jittered_inv_vp, cam.position, occlusion, clustered,
                                        /*enable_sky=*/true, ibl_views, atmos_skyview(),
                                        atmos_transmittance(), clouds, tuning);
    if (!hdr) {
        std::fprintf(stderr, "[fatal] lighting pass: %s\n", hdr.error().message.c_str());
        return false;
    }
    // Forward-blend transparent geometry over the lit HDR, depth-tested
    // against the opaque GBuffer depth.
    if (!fs.transparent_draws.empty()) {
        if (auto r = fr.transparent.add_to_graph(graph, *hdr, gbuffer->depth, render_extent,
                                                 fs.jittered_vp, fs.light, cam.position,
                                                 fs.transparent_draws, ibl_views, clustered,
                                                 settings.ibl.intensity);
            !r) {
            std::fprintf(stderr, "[fatal] transparent pass: %s\n", r.error().message.c_str());
            return false;
        }
    }
    // GPU particles (R10.3): simulated on the sim clock, drawn over the lit
    // HDR and the transparent geometry, depth-tested against the GBuffer (at
    // the render extent, before the upscaler resolves them with the rest).
    if (in.particles != nullptr) {
        in.particles->add_to_graph(graph, *in.registry,
                                   {frame_, &*gbuffer, *hdr, &clustered, &cam, fs.jittered_vp,
                                    fs.light, render_extent, in.sim_dt});
    }
    // R21: this frame's lit HDR is what next frame's reflections show.
    add_reflection_color(graph, *hdr);
    // R11: temporal resolve / upscale of the pre-tonemap HDR to the display
    // extent (frame_temporal.cpp); post-processing runs on its output.
    auto upscaled = add_upscale(graph, fs, *hdr, *gbuffer, draw_extent);
    if (!upscaled) {
        std::fprintf(stderr, "[fatal] upscaler: %s\n", upscaled.error().message.c_str());
        return false;
    }
    hdr = *upscaled;
    // R20: depth of field and motion blur on the resolved display image.
    hdr = add_cinematic_post(graph, in, *gbuffer, *hdr);
    // Bloom: blur the bright parts of the HDR and add them back before tonemap.
    if (settings.bloom.enabled) {
        const engine::renderer::BloomParams bloom_params{
            .threshold = settings.bloom.threshold,
            .knee = settings.bloom.knee,
            .intensity = settings.bloom.intensity,
            .radius = settings.bloom.radius};
        if (auto r = fr.bloom.add_to_graph(graph, *hdr, draw_extent, bloom_params); !r) {
            std::fprintf(stderr, "[fatal] bloom pass: %s\n", r.error().message.c_str());
            return false;
        }
    }
    // Auto-exposure: measure the (post-bloom) HDR luminance and adapt over
    // time. It keeps measuring in manual mode, so switching back to auto
    // starts from the current scene instead of a stale value.
    if (auto r = fr.exposure.add_to_graph(graph, *hdr, draw_extent, in.dt,
                                          settings.exposure.adaptation_s);
        !r) {
        std::fprintf(stderr, "[fatal] exposure pass: %s\n", r.error().message.c_str());
        return false;
    }
    const engine::rhi::ResourceHandle backbuffer = graph.import_image(
        "swapchain", swapchain.images()[image_index], swapchain.image_views()[image_index],
        swapchain.format(), present_extent, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        // The first barrier waits on the acquire semaphore's stage, so
        // every first touch (layout transition, the present copy) is ordered
        // after the presentation engine releases the image (audit A3).
        k_acquire_wait_stage, /*available_access=*/0,
        // The final PRESENT_SRC transition is ordered before the stage the
        // render-finished semaphore is signalled at, which present waits
        // on (issue #9).
        k_present_signal_stage);
    // The UI blends in gamma space through the swapchain's UNORM view when
    // the swapchain is mutable-format (ImGuiPass copies through a UNORM
    // scratch image otherwise).
    if (const auto unorm_views = swapchain.unorm_image_views(); !unorm_views.empty()) {
        graph.set_unorm_view(backbuffer, unorm_views[image_index]);
    }
    // In editor mode the frame resolves into the slot's offscreen viewport
    // image (sampled by the Viewport panel); otherwise straight into the
    // swapchain as before.
    engine::rhi::ResourceHandle scene_target = backbuffer;
#ifdef ENGINE_EDITOR
    engine::rhi::ResourceHandle viewport_handle{};
    if (editor_active_) {
        if (!ensure_viewport_target(fr, device, allocator, ldr_format_, draw_extent)) {
            std::fprintf(stderr, "[fatal] viewport target creation failed\n");
            return false;
        }
        viewport_handle = graph.import_image(
            "viewport", fr.viewport_image.handle(), fr.viewport_view.handle(),
            ldr_format_, draw_extent, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene_target = viewport_handle;
    }
#else
    (void)device;
    (void)allocator;
#endif
    // Tonemap to an intermediate LDR image viewed as the UNORM twin of the
    // target format (the shader encodes sRGB itself), then byte-copy it into
    // the target. The image is *stored* in the target's format, so the copy
    // into an sRGB swapchain is same-format: a UNORM -> sRGB copy made
    // MoltenVK cache a view of one CAMetalDrawable in the swapchain image and
    // crash at exit (rhi::add_byte_copy).
    const engine::rhi::ResourceHandle ldr_current = graph.create_transient_image(
        "ldr_current", ldr_format_, draw_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        graph.storage_format(scene_target));
    if (auto r = fr.tonemap.add_to_graph(graph, *hdr, ldr_current, draw_extent,
                                         fr.exposure.exposure_buffer_address(),
                                         settings.exposure, tonemap_look(in, fr));
        !r) {
        std::fprintf(stderr, "[fatal] tonemap pass: %s\n", r.error().message.c_str());
        return false;
    }
    if (auto r = engine::rhi::add_byte_copy(graph, "present_copy", ldr_current, scene_target,
                                            draw_extent);
        !r) {
        std::fprintf(stderr, "[fatal] %s\n", r.error().message.c_str());
        return false;
    }
#ifdef ENGINE_EDITOR
    // Editor UI: build the dockspace + panels (the Viewport panel samples
    // the offscreen scene image), then draw the UI into the swapchain as
    // the last pass. A null/empty draw list (editor disabled) adds no pass.
    {
        const ImDrawData* ui_draw_data = in.build_ui ? in.build_ui() : nullptr;
        if (auto r = fr.imgui.add_to_graph(graph, backbuffer, present_extent, ui_draw_data,
                                           viewport_handle, /*clear=*/editor_active_);
            !r) {
            std::fprintf(stderr, "[fatal] imgui pass: %s\n", r.error().message.c_str());
            return false;
        }
    }
#endif
    if (auto compiled = graph.compile(); !compiled) {
        std::fprintf(stderr, "[fatal] render graph compile failed: %s\n",
                     compiled.error().message.c_str());
        return false;
    }
    // Skin (compute) before the graph; record() adds the compute -> vertex
    // fetch barrier so the GBuffer sees the skinned vertices.
    if (!fs.dispatches.empty()) {
        QUAT_ZONE_C("skinning", engine::core::ProfileCategory::pass);
        static const engine::core::ProfileNameId skinning_zone =
            engine::core::intern_profile_name("skinning", engine::core::ProfileCategory::pass);
        const engine::rhi::GpuZoneScope gpu_zone(&gpu_profiler_, cmd, skinning_zone, "skinning");
        if (auto rs = fr.skinning.record(cmd, fs.dispatches); !rs) {
            std::fprintf(stderr, "[fatal] skinning pass: %s\n", rs.error().message.c_str());
            return false;
        }
    }
    graph.execute(cmd, &gpu_profiler_);
    return true;
}

} // namespace quat::app
