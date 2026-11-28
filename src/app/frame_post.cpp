// FrameRenderer, R20 cinematic post: the physical camera of the active scene
// camera drives depth of field, the motion-blur shutter and (optionally) the
// exposure; the look LUT named by grading.lut is loaded here as untrusted
// data. Order after the upscaler (display extent):
//   depth of field -> motion blur -> bloom -> exposure -> tonemap / grade.

#include "app/frame_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "app/asset_root.hpp"
#include "engine/renderer/photometry.hpp"
#include "engine/asset/cube_lut.hpp"
#include "engine/core/path_confine.hpp"
#include "engine/renderer/physical_camera.hpp"
#include "engine/rhi/device.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {
namespace {

namespace r = engine::renderer;

// The active camera's lens and the vertical FOV it renders with.
struct Lens {
    r::PhysicalCamera camera{};
    float             fov_y = 1.0471976F;
};

[[nodiscard]] Lens active_lens(const FrameInputs& in) {
    Lens out;
    // The projection's own FOV (P[1][1] = -1 / tan(fov / 2) after the Vulkan flip).
    const float p11 = std::abs(in.cam->projection[1][1]);
    if (p11 > 0.0F && std::isfinite(p11)) out.fov_y = 2.0F * std::atan(1.0F / p11);
    if (in.view_lens != nullptr) {
        out.camera = r::sanitized(*in.view_lens);
        return out;
    }
    const entt::registry* registry = in.registry;
    if (registry == nullptr) return out;
    const entt::entity e = engine::scene::find_active_camera(*registry);
    if (e == entt::null) return out;
    out.camera = r::sanitized(registry->get<const engine::scene::Camera>(e).lens);
    return out;
}

} // namespace

engine::rhi::ResourceHandle FrameRenderer::add_cinematic_post(
    engine::rhi::RenderGraph& graph, const FrameInputs& in,
    const engine::renderer::GBufferTargets& gbuffer, engine::rhi::ResourceHandle hdr) {
    DeferredFrame& fr = frames_[frame_];
    const FrameScene& fs = *in.scene;
    const engine::scene::CameraMatrices& cam = *in.cam;
    const Lens lens = active_lens(in);
    const float proj22 = cam.projection[2][2];
    const float proj32 = cam.projection[3][2];

    if (settings_.dof.enabled && fr.dof.valid()) {
        r::DofInputs dof;
        dof.color = hdr;
        dof.depth = gbuffer.depth;
        dof.display_extent = in.draw_extent;
        dof.render_extent = fs.render_extent;
        dof.coc = r::coc_params(lens.camera, lens.fov_y,
                                static_cast<float>(in.draw_extent.height));
        dof.max_radius_px = settings_.dof.max_radius_px;
        dof.rings = settings_.dof.rings;
        dof.proj22 = proj22;
        dof.proj32 = proj32;
        if (auto out = fr.dof.add_to_graph(graph, dof)) {
            hdr = *out;
        } else {
            std::fprintf(stderr, "[warn] depth of field: %s\n", out.error().message.c_str());
        }
    }
    const float fraction = r::shutter_fraction(lens.camera.shutter_s, in.dt) *
                           settings_.motion_blur.scale;
    if (settings_.motion_blur.enabled && fr.motion_blur.valid() && fraction > 0.0F) {
        r::MotionBlurInputs mb;
        mb.color = hdr;
        mb.velocity = gbuffer.velocity;
        mb.depth = gbuffer.depth;
        mb.display_extent = in.draw_extent;
        mb.render_extent = fs.render_extent;
        mb.shutter_fraction = fraction;
        mb.max_radius_px = settings_.motion_blur.max_radius_px;
        mb.samples = settings_.motion_blur.samples;
        mb.proj22 = proj22;
        mb.proj32 = proj32;
        mb.frame_index = static_cast<std::uint32_t>(fs.frame_number);
        if (auto out = fr.motion_blur.add_to_graph(graph, mb)) {
            hdr = *out;
        } else {
            std::fprintf(stderr, "[warn] motion blur: %s\n", out.error().message.c_str());
        }
    }
    return hdr;
}

void FrameRenderer::update_color_lut() {
    const std::string& wanted = settings_.grading.lut;
    if (wanted == lut_path_) return;
    lut_path_ = wanted;
    lut_.reset(); // frames in flight keep theirs (DeferredFrame::lut_in_use)
    if (wanted.empty()) return;
    auto file = engine::core::confine_to_root(project_root(), wanted);
    if (!file) {
        std::fprintf(stderr, "[warn] grading.lut '%s': %s\n", wanted.c_str(),
                     file.error().message.c_str());
        return;
    }
    auto table = engine::asset::load_cube_lut(*file);
    if (!table) {
        std::fprintf(stderr, "[warn] grading.lut: %s\n", table.error().message.c_str());
        return;
    }
    const VkDevice dev = device_->handle();
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    info.queueFamilyIndex = device_->queue_families().graphics;
    if (vkCreateCommandPool(dev, &info, nullptr, &pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[warn] grading.lut: could not create an upload command pool\n");
        return;
    }
    const engine::rhi::TransferContext transfer{dev, pool, device_->graphics_queue(),
                                                device_->max_sampler_anisotropy(),
                                                device_->texture_caps()};
    auto texture = r::ColorLutTexture::create(*allocator_, transfer, *table);
    vkDestroyCommandPool(dev, pool, nullptr);
    if (!texture) {
        std::fprintf(stderr, "[warn] grading.lut upload: %s\n", texture.error().message.c_str());
        return;
    }
    lut_ = std::make_shared<const r::ColorLutTexture>(std::move(*texture));
    std::fprintf(stderr, "[post] look LUT '%s' (%u^3) loaded\n", wanted.c_str(), lut_->size());
}

engine::renderer::TonemapLook FrameRenderer::tonemap_look(const FrameInputs& in,
                                                          DeferredFrame& frame) {
    update_color_lut();
    frame.lut_in_use = lut_;
    const Lens lens = active_lens(in);
    r::TonemapLook look;
    look.op = static_cast<r::TonemapOperator>(settings_.tonemap.op);
    look.grading = settings_.grading;
    look.film = settings_.film;
    look.lut = lut_.get();
    if (settings_.exposure.physical) {
        // R22.2.1: in HDR units (photometry.hpp), so the camera's EV100 means
        // what it means on a real camera (sunny 16 is EV100 15).
        look.physical_exposure = r::photometry::exposure_from_ev100_units(r::ev100(lens.camera));
    }
    look.pre_exposure = pre_exposure_;
    look.fov_y = lens.fov_y;
    look.time_s = cloud_time_s_;
    return look;
}

float FrameRenderer::choose_pre_exposure(const FrameInputs& in) const {
    // The exposure this frame will most likely be shown with: the previous
    // frame's auto exposure (read back from the host-visible state; one or
    // two frames old, which only matters as a scale shared by every pass of
    // this frame), the physical camera's, or the manual one, times the
    // compensation. Pre-exposed HDR then holds display-scale values whatever
    // the scene's light level (Lagarde & de Rousiers 2014).
    const r::ExposureSettings& e = settings_.exposure;
    float exposure = 1.0F;
    if (e.physical) {
        exposure = r::photometry::exposure_from_ev100_units(r::ev100(active_lens(in).camera));
    } else if (e.auto_exposure && exposure_state_) {
        exposure = exposure_state_->exposure();
    }
    exposure *= std::exp2(std::isfinite(e.compensation_ev) ? e.compensation_ev : 0.0F);
    // Keep the HDR's dynamic range usable even for absurd settings.
    return std::isfinite(exposure) ? std::clamp(exposure, 1e-12F, 1e12F) : 1.0F;
}

} // namespace quat::app
