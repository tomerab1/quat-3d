// FrameRenderer's temporal view and upscale (R11): render extent, jitter,
// view motion and camera cuts for gather(), and the upscaler declaration for
// record(). Kept out of frame_graph.cpp so the graph build only calls in.
//
// A8 settings: `upscaler` picks the render scale, sharpness and backend;
// `taa.enabled` off means no jitter, no history (every frame resolves as a
// reset) and native resolution; `taa.feedback` caps the resolve's
// accumulated sample weight at 1 / (1 - feedback).

#include "app/frame_renderer.hpp"

#include <cstdio>
#include <string>

#include "engine/core/profile.hpp"
#include "engine/renderer/jitter.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {

engine::renderer::UpscalerSettings FrameRenderer::current_upscaler() const {
    engine::renderer::UpscalerSettings s = settings_.upscaler;
    if (!settings_.taa.enabled) {
        s.mode = static_cast<int>(engine::renderer::UpscalerMode::native_aa);
        s.render_scale = 0.0F;
    }
    return s;
}

void FrameRenderer::begin_view(FrameScene& out, const engine::scene::CameraMatrices& cam,
                               VkExtent2D draw_extent, std::uint64_t frame_number) {
    // The scene renders at the upscaler's render extent with a sub-pixel Halton
    // jitter (one source for projection, velocity and the resolve); velocity
    // compares against last frame's UNjittered view-projection.
    const VkExtent2D render_extent =
        engine::renderer::upscaler_render_extent(draw_extent, current_upscaler());
    out.render_extent = render_extent;
    if (settings_.taa.enabled) {
        out.jitter = engine::renderer::halton_jitter(
            frame_number, render_extent,
            engine::renderer::jitter_phase_count(render_extent, draw_extent));
    }
    out.jittered_vp = engine::renderer::jitter_view_proj(cam.view_proj, out.jitter.ndc);
    out.jittered_inv_vp = glm::inverse(out.jittered_vp);
    out.motion = temporal_.begin_frame(cam.view_proj, out.jitter.ndc);
    out.reset_history = temporal_.take_history_reset();
}

void FrameRenderer::apply_texture_lod_bias(DeferredFrame& frame, VkExtent2D render,
                                           VkExtent2D display) {
    const float bias = engine::renderer::texture_lod_bias(render, display);
    if (auto r = frame.mesh.set_texture_lod_bias(bias); !r) {
        std::fprintf(stderr, "[warn] texture LOD bias: %s\n", r.error().message.c_str());
    }
    if (auto r = frame.transparent.set_texture_lod_bias(bias); !r) {
        std::fprintf(stderr, "[warn] texture LOD bias: %s\n", r.error().message.c_str());
    }
}

std::expected<engine::rhi::ResourceHandle, engine::core::Error>
FrameRenderer::add_upscale(engine::rhi::RenderGraph& graph, const FrameScene& scene,
                           engine::rhi::ResourceHandle hdr,
                           const engine::renderer::GBufferTargets& gbuffer, VkExtent2D display) {
    QUAT_ZONE_N("upscaler"); // backend choice + TAAU history / resolve declaration
    const engine::renderer::UpscalerSettings upscale = current_upscaler();
    const engine::renderer::UpscalerBackend wanted = engine::renderer::upscaler_backend(upscale);
    if (wanted != upscaler_->backend() && !backend_warned_) {
        backend_warned_ = true;
        std::fprintf(stderr, "[warn] upscaler backend %s unavailable; using %s\n",
                     std::string(engine::renderer::to_string(wanted)).c_str(),
                     std::string(engine::renderer::to_string(upscaler_->backend())).c_str());
    }
    engine::renderer::UpscalerInputs in;
    in.color = hdr;
    in.depth = gbuffer.depth;
    in.velocity = gbuffer.velocity;
    // The adapted luminance the previous frame's exposure pass left.
    in.exposure = exposure_state_->address();
    in.render_extent = scene.render_extent;
    in.display_extent = display;
    in.jitter = scene.jitter;
    in.sharpness = upscale.sharpness;
    in.reset = scene.reset_history; // a camera cut (reset_temporal())
    engine::renderer::apply_taa_settings(in, settings_.taa); // off: no history either
    if (non_finite_) {
        in.non_finite_counter =
            non_finite_->address(frame_, engine::renderer::NonFiniteSource::taa_history);
    }
    return upscaler_->add_to_graph(graph, frame_, in);
}

} // namespace quat::app
