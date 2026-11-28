// FrameRenderer setup, frame synchronisation and submission. The per-frame
// scene gather and render-graph build are in frame_graph.cpp, the
// QUAT_SCREENSHOT capture in frame_screenshot.cpp.

#include "app/frame_renderer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include "engine/core/profile.hpp"
#include "app/load_transfer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/formats.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/swapchain.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace quat::app {
namespace {

VkSemaphore make_semaphore(VkDevice vk_device) {
    VkSemaphore s = VK_NULL_HANDLE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(vk_device, &info, nullptr, &s);
    return s;
}

} // namespace

FrameRenderer::~FrameRenderer() {
    destroy_frame_sync();
}

bool FrameRenderer::create_passes(const engine::rhi::Device& device,
                                  engine::rhi::GpuAllocator& allocator,
                                  engine::rhi::PipelineCache& pipeline_cache,
                                  const engine::rhi::TransferContext& setup_transfer,
                                  const std::string& shader_dir,
                                  engine::rhi::Swapchain& swapchain, bool with_ui) {
    device_ = &device;
    allocator_ = &allocator;
    swapchain_ = &swapchain;
    editor_active_ = with_ui;

    const VkFormat swap_format = swapchain.format();
    // The swapchain is usually sRGB, which would double-encode if the tonemap
    // shader's explicit sRGB output were hardware-encoded again. So the
    // tonemap works in the matching UNORM format (the shader does the sRGB
    // encode), then a size-compatible copy moves the bits into the swapchain.
    ldr_format_ = engine::rhi::storage_unorm(swap_format);
    const VkFormat ldr_format = ldr_format_;

    // Temporal state shared by both slots (audit A2 / A11): the upscaler's
    // history and one adapted luminance, each advanced exactly once per frame.
    {
        auto upscaler = engine::renderer::create_upscaler(
            engine::renderer::UpscalerBackend::taau, device, allocator, pipeline_cache,
            shader_dir, k_frames_in_flight);
        auto velocity = engine::renderer::VelocityPass::create(device, pipeline_cache, shader_dir);
        if (!upscaler || !velocity) {
            std::fprintf(stderr, "[fatal] upscaler / velocity pass creation failed: %s\n",
                         (!upscaler ? upscaler.error() : velocity.error()).message.c_str());
            return false;
        }
        upscaler_ = std::move(*upscaler);
        velocity_ = std::move(*velocity);
    }
    auto exposure_state = engine::renderer::ExposureState::create(device, allocator);
    // Per-frame-in-flight deferred chains (GBuffer -> lighting -> tonemap). Each
    // slot owns its passes' descriptor buffers and a transient image pool, so
    // building one frame's graph never disturbs the other in-flight frame.
    bool frames_ok = exposure_state.has_value();
    if (!exposure_state) {
        std::fprintf(stderr, "[fatal] exposure state creation failed: %s\n",
                     exposure_state.error().message.c_str());
    } else {
        exposure_state_ = *exposure_state;
    }
    if (frames_ok) {
        if (auto csm = engine::renderer::CsmShared::create(device, allocator)) {
            csm_shared_ = std::move(*csm);
        } else {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] shadow map creation failed: %s\n",
                         csm.error().message.c_str());
        }
    }
    for (auto& f : frames_) {
        if (!frames_ok) break;
        auto mesh = engine::renderer::MeshPass::create(device, allocator, pipeline_cache,
                                                       setup_transfer, shader_dir);
        auto light = engine::renderer::LightingPass::create(device, allocator, pipeline_cache,
                                                            shader_dir);
        auto tone = engine::renderer::TonemapPass::create(device, allocator, pipeline_cache,
                                                          shader_dir, ldr_format);
        auto skin = engine::renderer::SkinningPass::create(device, allocator, pipeline_cache,
                                                           shader_dir);
        auto shadow = engine::renderer::ShadowPass::create(device, allocator, pipeline_cache,
                                                           shader_dir, csm_shared_);
        auto transp = engine::renderer::TransparentPass::create(device, allocator, pipeline_cache,
                                                                setup_transfer, shader_dir);
        auto bloom = engine::renderer::BloomPass::create(device, allocator, pipeline_cache,
                                                         shader_dir);
        auto expo = engine::renderer::ExposurePass::create(device, allocator, pipeline_cache,
                                                           shader_dir, exposure_state_);
#ifdef ENGINE_EDITOR
        if (with_ui) {
            auto ui = engine::renderer::ImGuiPass::create(device, allocator, pipeline_cache,
                                                          setup_transfer, shader_dir,
                                                          swapchain.format());
            if (!ui) {
                frames_ok = false;
                std::fprintf(stderr, "[fatal] imgui pass creation failed: %s\n",
                             ui.error().message.c_str());
                break;
            }
            f.imgui = std::move(*ui);
        }
#endif
        if (!mesh || !light || !tone || !skin || !shadow || !transp || !bloom || !expo) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] deferred pass creation failed: %s\n",
                         (!mesh    ? mesh.error()
                          : !light  ? light.error()
                          : !tone   ? tone.error()
                          : !skin   ? skin.error()
                          : !shadow ? shadow.error()
                          : !transp ? transp.error()
                          : !bloom  ? bloom.error()
                                    : expo.error())
                             .message.c_str());
            break;
        }
        f.mesh = std::move(*mesh);
        f.lighting = std::move(*light);
        f.tonemap = std::move(*tone);
        f.skinning = std::move(*skin);
        f.shadow = std::move(*shadow);
        f.transparent = std::move(*transp);
        f.bloom = std::move(*bloom);
        f.exposure = std::move(*expo);
        f.pool = engine::rhi::TransientImagePool(device, allocator);
        auto cull = engine::renderer::LightCullPass::create(device, allocator, pipeline_cache,
                                                            shader_dir);
        if (!cull) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] light cull pass creation failed: %s\n",
                         cull.error().message.c_str());
            break;
        }
        f.light_cull = std::move(*cull);
        auto wind = engine::renderer::WindFieldPass::create(device, allocator, pipeline_cache,
                                                            shader_dir);
        if (!wind) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] wind field pass creation failed: %s\n",
                         wind.error().message.c_str());
            break;
        }
        f.wind = std::move(*wind);
        auto cloud_shadow = engine::renderer::CloudShadowPass::create(device, allocator,
                                                                      pipeline_cache, shader_dir);
        if (!cloud_shadow) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] cloud shadow pass creation failed: %s\n",
                         cloud_shadow.error().message.c_str());
            break;
        }
        f.cloud_shadow = std::move(*cloud_shadow);
        // Hi-Z and GTAO are optional (they need extended storage formats):
        // without them the cascades are fitted on the CPU and there is no AO.
        if (auto hiz = engine::renderer::HiZPass::create(device, allocator, pipeline_cache,
                                                         shader_dir)) {
            f.hiz = std::move(*hiz);
        } else if (&f == &frames_[0]) {
            std::fprintf(stderr, "[warn] hi-z pass unavailable (CPU cascade fit): %s\n",
                         hiz.error().message.c_str());
        }
        // R20 depth of field + motion blur, optional for the same reason.
        if (auto dof = engine::renderer::DofPass::create(device, allocator, pipeline_cache,
                                                         shader_dir)) {
            f.dof = std::move(*dof);
        } else if (&f == &frames_[0]) {
            std::fprintf(stderr, "[warn] depth of field unavailable: %s\n",
                         dof.error().message.c_str());
        }
        if (auto mb = engine::renderer::MotionBlurPass::create(device, allocator, pipeline_cache,
                                                               shader_dir)) {
            f.motion_blur = std::move(*mb);
        } else if (&f == &frames_[0]) {
            std::fprintf(stderr, "[warn] motion blur unavailable: %s\n",
                         mb.error().message.c_str());
        }
        if (auto gtao = engine::renderer::GtaoPass::create(device, allocator, pipeline_cache,
                                                           shader_dir)) {
            f.gtao = std::move(*gtao);
        } else if (&f == &frames_[0]) {
            std::fprintf(stderr, "[warn] gtao pass unavailable (no AO): %s\n",
                         gtao.error().message.c_str());
        }
    }

    // Debug NaN/Inf accounting at the bloom / TAA guards (issue #45): on in
    // debug builds, or with QUAT_NON_FINITE_CHECK=1 (CI screenshots).
#ifndef NDEBUG
    const bool count_non_finite = true;
#else
    const char* check_env = std::getenv("QUAT_NON_FINITE_CHECK");
    const bool count_non_finite = check_env != nullptr && check_env[0] == '1';
#endif
    if (frames_ok && count_non_finite) {
        if (auto counters =
                engine::renderer::NonFiniteCounters::create(device, allocator, k_frames_in_flight);
            counters) {
            non_finite_ = std::move(*counters);
            for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
                using engine::renderer::NonFiniteSource;
                frames_[i].bloom.set_non_finite_counter(
                    non_finite_->address(i, NonFiniteSource::bloom_prefilter));
            }
            // The TAAU resolve is shared by the slots; add_upscale passes it
            // the current slot's taa_history counter with each frame's inputs.
        } else {
            std::fprintf(stderr, "[warn] non-finite counters unavailable: %s\n",
                         counters.error().message.c_str());
        }
    }
    return frames_ok;
}

#ifdef ENGINE_EDITOR
void FrameRenderer::reload_ui_fonts() {
    if (!editor_active_ || device_ == nullptr) return;
    // Both frames' atlases are replaced: no submitted frame may still read them.
    if (const VkResult r = vkDeviceWaitIdle(device_->handle()); r != VK_SUCCESS) {
        std::fprintf(stderr, "[warn] UI font reload skipped: vkDeviceWaitIdle failed (%d)\n",
                     static_cast<int>(r));
        return;
    }
    (void)with_load_transfer(*device_, [&](const engine::rhi::TransferContext& transfer) {
        for (auto& f : frames_) {
            if (auto r = f.imgui.upload_font_atlas(transfer); !r) {
                std::fprintf(stderr, "[warn] UI font reload failed: %s\n",
                             r.error().message.c_str());
            }
        }
    });
}
#endif

void FrameRenderer::create_environment(engine::scene::Scene& scene,
                                       engine::rhi::PipelineCache& pipeline_cache,
                                       const std::string& shader_dir) {
    const engine::rhi::Device& device = *device_;
    engine::rhi::GpuAllocator& allocator = *allocator_;
    // Physically-based atmosphere (Phase 11.1): static transmittance/multi-
    // scatter LUTs now, sky-view LUT refreshed whenever the sun moves. The
    // procedural sky remains the fallback if creation fails.
    {
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
        const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : dlv) {
            sun_to = -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
            break;
        }
        if (auto atmos = engine::renderer::AtmospherePass::create(device, allocator,
                                                                  pipeline_cache, shader_dir);
            atmos) {
            atmosphere_ = std::move(*atmos);
            if (auto r = atmosphere_->ensure_skyview(sun_to); !r) {
                std::fprintf(stderr, "[warn] sky-view LUT failed: %s\n",
                             r.error().message.c_str());
                atmosphere_.reset();
            } else {
                std::fprintf(stderr, "[info] atmosphere LUTs ready\n");
            }
        } else {
            std::fprintf(stderr, "[warn] atmosphere create failed: %s\n",
                         atmos.error().message.c_str());
        }
    }

    // Image-based lighting: a dynamic environment probe (11.3). create() runs
    // one blocking full cycle so ambient/reflections are complete from frame
    // one; afterwards the probe re-renders incrementally, one compute step per
    // frame inside the render graph, so the IBL tracks the moving sun and
    // drifting clouds with no rebake hitches. Falls back to flat ambient on
    // failure.
    {
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
        const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : dlv) {
            sun_to = -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
            break;
        }
        engine::renderer::IblCycleParams initial{};
        initial.sun_to = sun_to;
        initial.skyview = atmosphere_ ? atmosphere_->skyview_view() : VK_NULL_HANDLE;
        initial.transmittance = atmosphere_ ? atmosphere_->transmittance_view() : VK_NULL_HANDLE;
        initial.clouds = current_clouds();
        if (auto dyn = engine::renderer::DynamicIbl::create(device, allocator, pipeline_cache,
                                                            shader_dir, initial);
            dyn) {
            dynamic_ibl_ = std::move(*dyn);
            std::fprintf(stderr, "[info] dynamic IBL probe ready\n");
        } else {
            std::fprintf(stderr, "[warn] dynamic IBL create failed: %s\n",
                         dyn.error().message.c_str());
        }
    }
}

bool FrameRenderer::create_frame_sync(std::uint64_t gpu_timeout_ns) {
    const VkDevice vk_device = device_->handle();
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device_->queue_families().graphics;
        vkCreateCommandPool(vk_device, &pool_info, nullptr, &command_pool_);
    }
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = k_frames_in_flight;
        vkAllocateCommandBuffers(vk_device, &alloc, command_buffers_.data());
    }
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        image_available_[i] = make_semaphore(vk_device);
    }
    if (auto fences = fences_.create(
            make_vulkan_queue_sync_ops(vk_device, device_->graphics_queue()), k_frames_in_flight,
            gpu_timeout_ns);
        !fences) {
        std::fprintf(stderr, "[fatal] frame fence creation failed: %s\n",
                     describe_gpu_fault(fences.error()).c_str());
        return false;
    }
    render_finished_.assign(swapchain_->image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished_) {
        s = make_semaphore(vk_device);
    }
    create_gpu_profiler();
    return true;
}

void FrameRenderer::destroy_frame_sync() {
    material_overrides_.clear(); // the device is idle: no frame reads them
    gpu_profiler_.destroy();
    if (device_ == nullptr || command_pool_ == VK_NULL_HANDLE) return;
    const VkDevice vk_device = device_->handle();
    for (VkSemaphore s : render_finished_) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    render_finished_.clear();
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        vkDestroySemaphore(vk_device, image_available_[i], nullptr);
        image_available_[i] = VK_NULL_HANDLE;
    }
    fences_.destroy();
    vkDestroyCommandPool(vk_device, command_pool_, nullptr);
    command_pool_ = VK_NULL_HANDLE;
}

void FrameRenderer::recreate_render_finished() {
    const VkDevice vk_device = device_->handle();
    for (VkSemaphore s : render_finished_) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    render_finished_.assign(swapchain_->image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished_) {
        s = make_semaphore(vk_device);
    }
}

bool FrameRenderer::ensure_swapchain(VkExtent2D pixel_extent) {
    if (!needs_recreate_) return true;
    if (auto r = swapchain_->recreate(pixel_extent); !r) {
        std::fprintf(stderr, "[fatal] swapchain recreate failed: %s\n",
                     r.error().message.c_str());
        return false;
    }
    recreate_render_finished();
    needs_recreate_ = false;
    return true;
}

std::expected<std::uint32_t, GpuFault> FrameRenderer::wait_for_slot() {
    {
        QUAT_ZONE_C("wait_gpu", engine::core::ProfileCategory::wait);
        // Finite: a GPU that never finishes the frame is reported as a hang
        // instead of freezing the loop (issue #110).
        if (auto waited = fences_.wait(frame_); !waited) {
            std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(waited.error()).c_str());
            return std::unexpected(waited.error());
        }
    }
    collect_gpu_timings();
    if (non_finite_) {
        const auto counts = non_finite_->collect(frame_);
        bool any = false;
        for (const std::uint32_t c : counts) any = any || c != 0;
        if (any) {
            using engine::renderer::NonFiniteSource;
            std::fprintf(stderr,
                         "[warn] non-finite HDR values replaced last frame: %s %u, %s %u "
                         "(a NaN/Inf source upstream, issue #45)\n",
                         engine::renderer::non_finite_source_name(NonFiniteSource::bloom_prefilter),
                         counts[0],
                         engine::renderer::non_finite_source_name(NonFiniteSource::taa_history),
                         counts[1]);
        }
    }
    return frame_;
}

FrameRenderer::Acquire FrameRenderer::acquire_and_begin() {
    const VkDevice vk_device = device_->handle();
    image_index_ = 0;
    VkResult acquire = VK_SUCCESS;
    {
        QUAT_ZONE_C("acquire_image", engine::core::ProfileCategory::wait);
        acquire = vkAcquireNextImageKHR(vk_device, swapchain_->handle(), UINT64_MAX,
                                        image_available_[frame_], VK_NULL_HANDLE, &image_index_);
    }
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
        return Acquire::retry;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        const GpuFault fault =
            fences_.report(classify_gpu_fault(acquire, "vkAcquireNextImageKHR")).error();
        std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(fault).c_str());
        return Acquire::failed;
    }

    if (auto reset = fences_.reset(frame_); !reset) {
        std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(reset.error()).c_str());
        return Acquire::failed;
    }

    VkCommandBuffer cmd = command_buffers_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    gpu_profiler_.begin_frame(cmd, frame_, engine::core::profiler().frame_index());

    // This frame's deferred graph builds into the slot's own resources. The
    // slot's fence was waited above, so resetting its transient pool and
    // rebuilding the passes' descriptor buffers does not race the other
    // in-flight frame.
    frames_[frame_].pool.reset();
    return Acquire::ok;
}

VkExtent2D FrameRenderer::present_extent() const {
    return swapchain_->extent();
}

CaptureSource FrameRenderer::capture_source() const {
    // Remote screenshots copy the final scene image (no editor UI).
    CaptureSource capture{swapchain_->images()[image_index_], swapchain_->format(),
                          swapchain_->extent(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
#ifdef ENGINE_EDITOR
    if (editor_active_) {
        capture = {frames_[frame_].viewport_image.handle(), ldr_format_, draw_extent_,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
#endif
    return capture;
}

bool FrameRenderer::submit_and_present(bool screenshot_frame) {
    VkCommandBuffer cmd = command_buffers_[frame_];
    const bool screenshot_recorded =
        screenshot_frame && screenshot_path_.has_value() && record_screenshot_copy(cmd);

    gpu_profiler_.end_frame(cmd);
    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo wait{};
    wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    wait.semaphore = image_available_[frame_];
    wait.stageMask = k_acquire_wait_stage;

    VkSemaphoreSubmitInfo signal{};
    signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signal.semaphore = render_finished_[image_index_];
    // Chains onto the PRESENT_SRC transitions, whose second scope is this
    // stage; the swapchain image's writes are in those barriers' first scope.
    // The frame fence covers the rest of the frame's work.
    signal.stageMask = k_present_signal_stage;

    VkCommandBufferSubmitInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd_info.commandBuffer = cmd;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd_info;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    {
        QUAT_ZONE_N("queue_submit");
        // A failed submit leaves the slot's fence signalled (FrameFences), so
        // nothing waits on it forever; the frame is not presented (its
        // render-finished semaphore will never be signalled) and the loop
        // stops on the fault.
        if (auto submitted = fences_.submit(frame_, submit); !submitted) {
            std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(submitted.error()).c_str());
            return false;
        }
    }

    VkSwapchainKHR sc = swapchain_->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[image_index_];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &image_index_;
    VkResult present_result = VK_SUCCESS;
    {
        QUAT_ZONE_C("present", engine::core::ProfileCategory::wait);
        present_result = vkQueuePresentKHR(device_->present_queue(), &present);
    }
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
    } else if (present_result != VK_SUCCESS) {
        const GpuFault fault =
            fences_.report(classify_gpu_fault(present_result, "vkQueuePresentKHR")).error();
        std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(fault).c_str());
        return false;
    }

    if (screenshot_recorded) {
        // The frame's own fence, with the same hang timeout as the loop.
        if (auto waited = fences_.wait(frame_); !waited) {
            std::fprintf(stderr, "[fatal] %s\n", describe_gpu_fault(waited.error()).c_str());
            return false;
        }
        write_screenshot();
    }

    frame_ = (frame_ + 1) % k_frames_in_flight;
    return true;
}

} // namespace quat::app
