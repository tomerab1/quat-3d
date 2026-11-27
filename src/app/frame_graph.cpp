// FrameRenderer, per frame: gathers the ticked scene into draw lists and builds
// the render graph. Pass order (graph declaration order):
//   sky-view LUT update, dynamic IBL step, shadow, GBuffer, terrain, light
//   culling, lighting, transparent, bloom, exposure, tonemap, TAA, editor UI;
//   GPU skinning is recorded before the graph executes.

#include "app/frame_renderer.hpp"
#include "app/grass_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <glm/gtc/matrix_transform.hpp>

#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/shadow_fit.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/instanced_mesh.hpp"
#include "engine/scene/scene.hpp"
#ifdef ENGINE_EDITOR
#include "engine/editor/editor.hpp"
#endif

namespace quat::app {
namespace {

#ifdef ENGINE_EDITOR
// (Re)creates the slot's offscreen viewport image at `extent`. Safe to replace
// here: the slot's fence was waited before its frame is rebuilt. The image uses
// the swapchain format so the TAA resolve copy and the UI's sRGB sampling
// behave exactly as the direct-to-swapchain path.
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

    auto joints = allocator.create_buffer(
        joints_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    if (!joints) return nullptr;
    auto verts = allocator.create_buffer(
        verts_size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO, 0);
    if (!verts) return nullptr;

    SkinnedEntityGpu g;
    g.joints = std::move(*joints);
    g.skinned_verts = std::move(*verts);
    g.joints_addr = engine::rhi::buffer_device_address(vk_device, g.joints);
    g.skinned_addr = engine::rhi::buffer_device_address(vk_device, g.skinned_verts);
    g.vertex_count = mesh.vertex_count;
    g.joint_count = joint_count;
    frame.skin_gpu[entity] = std::move(g);
    return &frame.skin_gpu[entity];
}

} // namespace

engine::renderer::CloudSettings FrameRenderer::current_clouds() const {
    engine::renderer::CloudSettings c{};
#ifdef ENGINE_EDITOR
    if (render_settings_ != nullptr) c = render_settings_->clouds;
#endif
    c.time_s = cloud_time_s_;
    return c;
}

FrameScene FrameRenderer::gather(engine::scene::Scene& scene,
                                 const engine::scene::CameraMatrices& cam,
                                 VkExtent2D draw_extent, std::uint64_t frame_number) {
    const VkDevice vk_device = device_->handle();
    engine::rhi::GpuAllocator& allocator = *allocator_;
    DeferredFrame& fr = frames_[frame_];
    FrameScene out;

    // TAA: jitter the projection sub-pixel each frame (Halton). The whole
    // frame renders with the jittered view-proj; the resolve accumulates
    // into the shared history, reprojecting with the UNjittered
    // cam.view_proj (audit A14).
    const glm::vec2 jitter =
        engine::renderer::taa_jitter(frame_number, draw_extent.width, draw_extent.height);
    glm::mat4 jitter_mat(1.0F);
    jitter_mat[3][0] = jitter.x;
    jitter_mat[3][1] = jitter.y;
    out.jittered_vp = jitter_mat * cam.view_proj;
    out.jittered_inv_vp = glm::inverse(out.jittered_vp);

    engine::renderer::DirectionalLightParams& light = out.light;
    const auto light_view = scene.registry().view<const engine::scene::DirectionalLight>();
    for (const entt::entity e : light_view) {
        const auto& dl = light_view.get<const engine::scene::DirectionalLight>(e);
        light.direction = glm::vec4(-glm::normalize(dl.direction), 0.0F);
        light.color = glm::vec4(dl.color, dl.intensity);
        break;
    }

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
        item.material = mr.material.valid() ? &*mr.material : nullptr;
        item.model = t.world;

        const auto* sm = scene.registry().try_get<engine::scene::SkinnedMesh>(e);
        if (item.mesh != nullptr && item.mesh->skinned() && sm != nullptr &&
            !sm->joint_matrices.empty()) {
            SkinnedEntityGpu* g = ensure_skin_gpu(
                fr, e, *item.mesh, vk_device, allocator,
                static_cast<std::uint32_t>(sm->joint_matrices.size()));
            if (g != nullptr) {
                const VkDeviceSize joint_bytes = sm->joint_matrices.size() * sizeof(glm::mat4);
                std::memcpy(g->joints.mapped(), sm->joint_matrices.data(), joint_bytes);
                // Sequential-write host memory may be non-coherent (S1.A15).
                if (auto flushed = g->joints.flush(0, joint_bytes); !flushed) {
                    std::fprintf(stderr, "[warn] joint matrix flush: %s\n",
                                 flushed.error().message.c_str());
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
                out.dispatches.push_back(d);
                item.skinned_vertices = g->skinned_verts.handle();
                // Skinned vertices are already in skin space (glTF ignores the
                // mesh node transform), so draw them with an identity model.
                item.model = glm::mat4(1.0F);
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

    // Shadow casters: all opaques plus transmissive glass (solid surfaces —
    // they block the sun like opaque geometry; alpha-blended items do not cast).
    out.shadow_draws = draws;
    for (const engine::renderer::DrawItem& d : transparent_draws) {
        if (d.material != nullptr &&
            (d.material->params.flags & engine::asset::material_transmission) != 0) {
            out.shadow_draws.push_back(d);
        }
    }

    // Directional shadow: an orthographic light view fit to the camera's
    // view, or to a fixed box around the origin (the small demos sit there).
    const glm::vec3 sun_dir = -glm::normalize(glm::vec3(light.direction)); // travel direction
    if (shadow_view_distance_ > 0.0F) {
        const float fov_y = 2.0F * std::atan(1.0F / std::abs(cam.projection[1][1]));
        const float aspect = std::abs(cam.projection[1][1] / cam.projection[0][0]);
        out.light_view_proj = engine::renderer::fit_shadow_to_view(
            glm::inverse(cam.view), fov_y, aspect, sun_dir,
            {.distance = shadow_view_distance_,
             .caster_reach = shadow_view_distance_,
             .map_size = engine::renderer::shadow_map_size});
        return out;
    }
    const glm::vec3 shadow_center(0.0F, 1.5F, 0.0F);
    constexpr float shadow_radius = 14.0F;
    const glm::vec3 light_eye = shadow_center - sun_dir * (shadow_radius * 1.5F);
    const glm::mat4 shadow_view =
        glm::lookAt(light_eye, shadow_center, glm::vec3(0.0F, 1.0F, 0.0F));
    const glm::mat4 shadow_proj = glm::ortho(-shadow_radius, shadow_radius, -shadow_radius,
                                             shadow_radius, 0.1F, shadow_radius * 3.5F);
    out.light_view_proj = shadow_proj * shadow_view;
    return out;
}

bool FrameRenderer::record(const FrameInputs& in) {
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
    // the sun moved (no pipeline stall on sun drags); the dynamic IBL
    // probe advances one step per frame and flips when a cycle completes.
    const glm::vec3 sun_to = glm::vec3(fs.light.direction);
    if (atmosphere_) {
        if (auto r = atmosphere_->add_skyview_update_to_graph(graph, sun_to); !r) {
            std::fprintf(stderr, "[warn] sky-view update: %s\n", r.error().message.c_str());
        }
    }
    if (dynamic_ibl_) {
        engine::renderer::IblCycleParams cycle{};
        cycle.sun_to = sun_to;
        cycle.skyview = atmos_skyview();
        cycle.transmittance = atmos_transmittance();
        cycle.clouds = current_clouds();
        if (auto r = dynamic_ibl_->add_step_to_graph(graph, cycle); !r) {
            std::fprintf(stderr, "[warn] ibl step: %s\n", r.error().message.c_str());
        }
    }
    const engine::renderer::IblViewSet ibl_views =
        dynamic_ibl_ ? dynamic_ibl_->views() : engine::renderer::IblViewSet{};

    // R9.1: draws sharing mesh + material collapse into instanced draws;
    // R9.2: InstancedMesh placements join them, one draw per (mesh,
    // material), culled per instance.
    const engine::scene::FrameDrawLists batches = engine::scene::build_frame_draw_lists(
        *in.registry, fs.draws, fs.shadow_draws, cam.view_proj, fs.light_view_proj);
    const engine::renderer::InstancedDrawList& shadow_batches = batches.shadow;
    const engine::renderer::InstancedDrawList& opaque_batches = batches.opaque;
    // A5 debug.stats + the editor's Stats panel: draws after batching.
    draw_count_ = engine::scene::frame_draw_count(batches, fs.transparent_draws.size());
    auto shadow = fr.shadow.add_to_graph(graph, fs.light_view_proj, shadow_batches);
    if (!shadow) {
        std::fprintf(stderr, "[fatal] shadow pass: %s\n", shadow.error().message.c_str());
        return false;
    }
    auto gbuffer = fr.mesh.add_to_graph(graph, draw_extent, fs.jittered_vp, opaque_batches);
    if (!gbuffer) {
        std::fprintf(stderr, "[fatal] gbuffer pass: %s\n", gbuffer.error().message.c_str());
        return false;
    }
    if (in.terrain != nullptr) {
        in.terrain->add_to_graph(graph, *gbuffer, draw_extent, fs.jittered_vp, cam.position,
                                 in.terrain_anchor, in.terrain_snowline);
    }
    if (in.grass != nullptr) {
        in.grass->add_to_graph(graph, frame_, *in.registry, *gbuffer, draw_extent, fs.jittered_vp,
                               cam, in.terrain, in.terrain_anchor, in.sim_dt);
    }
    // Clustered light culling (R10.1). A camera the clusters cannot
    // represent (non-perspective / no finite far plane) renders unlit by
    // point lights rather than stopping the frame loop.
    engine::renderer::ClusteredLights clustered{};
    if (auto c = fr.light_cull.add_to_graph(graph, cam.view, cam.projection, fs.point_lights)) {
        clustered = *c;
    } else {
        std::fprintf(stderr, "[warn] light cull: %s\n", c.error().message.c_str());
    }
    auto hdr = fr.lighting.add_to_graph(graph, *gbuffer, draw_extent, fs.light,
                                        fs.jittered_inv_vp, cam.position, *shadow,
                                        fs.light_view_proj, clustered,
                                        /*enable_sky=*/true, ibl_views, atmos_skyview(),
                                        atmos_transmittance(), current_clouds());
    if (!hdr) {
        std::fprintf(stderr, "[fatal] lighting pass: %s\n", hdr.error().message.c_str());
        return false;
    }
    // Forward-blend transparent geometry over the lit HDR, depth-tested
    // against the opaque GBuffer depth.
    if (!fs.transparent_draws.empty()) {
        if (auto r = fr.transparent.add_to_graph(graph, *hdr, gbuffer->depth, draw_extent,
                                                 fs.jittered_vp, fs.light, cam.position,
                                                 fs.transparent_draws, ibl_views, clustered);
            !r) {
            std::fprintf(stderr, "[fatal] transparent pass: %s\n", r.error().message.c_str());
            return false;
        }
    }
    // Bloom: blur the bright parts of the HDR and add them back before tonemap.
    engine::renderer::BloomParams bloom_params{};
#ifdef ENGINE_EDITOR
    if (render_settings_ != nullptr) {
        bloom_params.threshold = render_settings_->bloom_threshold;
        bloom_params.knee = render_settings_->bloom_knee;
        bloom_params.intensity = render_settings_->bloom_intensity;
        bloom_params.radius = render_settings_->bloom_radius;
    }
#endif
    if (auto r = fr.bloom.add_to_graph(graph, *hdr, draw_extent, bloom_params); !r) {
        std::fprintf(stderr, "[fatal] bloom pass: %s\n", r.error().message.c_str());
        return false;
    }
    // Auto-exposure: measure the (post-bloom) HDR luminance and adapt over time.
    if (auto r = fr.exposure.add_to_graph(graph, *hdr, draw_extent, in.dt); !r) {
        std::fprintf(stderr, "[fatal] exposure pass: %s\n", r.error().message.c_str());
        return false;
    }
    const engine::rhi::ResourceHandle backbuffer = graph.import_image(
        "swapchain", swapchain.images()[image_index], swapchain.image_views()[image_index],
        swapchain.format(), present_extent, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        // The first barrier waits on the acquire semaphore's stage, so
        // every first touch (layout transition, TAA's copy) is ordered
        // after the presentation engine releases the image (audit A3).
        k_acquire_wait_stage);
    // In editor mode the frame resolves into the slot's offscreen viewport
    // image (sampled by the Viewport panel); otherwise straight into the
    // swapchain as before.
    engine::rhi::ResourceHandle scene_target = backbuffer;
#ifdef ENGINE_EDITOR
    engine::rhi::ResourceHandle viewport_handle{};
    if (editor_active_) {
        if (!ensure_viewport_target(fr, device, allocator, swapchain.format(), draw_extent)) {
            std::fprintf(stderr, "[fatal] viewport target creation failed\n");
            return false;
        }
        viewport_handle = graph.import_image(
            "viewport", fr.viewport_image.handle(), fr.viewport_view.handle(),
            swapchain.format(), draw_extent, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene_target = viewport_handle;
    }
#else
    (void)device;
    (void)allocator;
#endif
    // Tonemap to an intermediate LDR image; TAA resolves it into the target.
    const engine::rhi::ResourceHandle ldr_current = graph.create_transient_image(
        "ldr_current", ldr_format_, draw_extent,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (auto r = fr.tonemap.add_to_graph(graph, *hdr, ldr_current, draw_extent,
                                         fr.exposure.exposure_buffer_address());
        !r) {
        std::fprintf(stderr, "[fatal] tonemap pass: %s\n", r.error().message.c_str());
        return false;
    }
    // Temporal anti-aliasing: resolve the jittered frame against history.
    if (auto r = fr.taa.add_to_graph(graph, ldr_current, gbuffer->depth, scene_target,
                                     draw_extent, cam.view_proj);
        !r) {
        std::fprintf(stderr, "[fatal] taa pass: %s\n", r.error().message.c_str());
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
        if (auto rs = fr.skinning.record(cmd, fs.dispatches); !rs) {
            std::fprintf(stderr, "[fatal] skinning pass: %s\n", rs.error().message.c_str());
            return false;
        }
    }
    graph.execute(cmd);
    return true;
}

} // namespace quat::app
