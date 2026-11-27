// FrameRenderer setup, frame synchronisation, submission and screenshots.
// The per-frame scene gather and render-graph build are in frame_graph.cpp.

#include "app/frame_renderer.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>

#include "engine/rhi/device.hpp"
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
    // The swapchain is usually sRGB, which cannot be a storage image (TAA writes
    // one) and would double-encode if the tonemap shader's explicit sRGB output
    // were hardware-encoded again. So tonemap + post-process work in the matching
    // UNORM format (the shader does the sRGB encode), then a size-compatible copy
    // moves the bits into the sRGB swapchain.
    ldr_format_ = [swap_format] {
        switch (swap_format) {
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            default: return swap_format;
        }
    }();
    const VkFormat ldr_format = ldr_format_;

    // Temporal state shared by both slots (audit A2 / A11): one TAA history and
    // one adapted luminance, each advanced exactly once per frame.
    taa_history_ =
        std::make_shared<engine::renderer::TaaHistory>(device, allocator, k_frames_in_flight);
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
                                                           shader_dir);
        auto transp = engine::renderer::TransparentPass::create(device, allocator, pipeline_cache,
                                                                setup_transfer, shader_dir);
        auto bloom = engine::renderer::BloomPass::create(device, allocator, pipeline_cache,
                                                         shader_dir);
        auto expo = engine::renderer::ExposurePass::create(device, allocator, pipeline_cache,
                                                           shader_dir, exposure_state_);
        auto taa = engine::renderer::TaaPass::create(device, allocator, pipeline_cache, shader_dir,
                                                     ldr_format, taa_history_);
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
        if (!mesh || !light || !tone || !skin || !shadow || !transp || !bloom || !expo || !taa) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] deferred pass creation failed: %s\n",
                         (!mesh    ? mesh.error()
                          : !light  ? light.error()
                          : !tone   ? tone.error()
                          : !skin   ? skin.error()
                          : !shadow ? shadow.error()
                          : !transp ? transp.error()
                          : !bloom  ? bloom.error()
                          : !expo   ? expo.error()
                                    : taa.error())
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
        f.taa = std::move(*taa);
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
                frames_[i].taa.set_non_finite_counter(
                    non_finite_->address(i, NonFiniteSource::taa_history));
            }
        } else {
            std::fprintf(stderr, "[warn] non-finite counters unavailable: %s\n",
                         counters.error().message.c_str());
        }
    }
    return frames_ok;
}

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

void FrameRenderer::create_frame_sync() {
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
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk_device, &fence_info, nullptr, &in_flight_[i]);
    }
    render_finished_.assign(swapchain_->image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished_) {
        s = make_semaphore(vk_device);
    }
}

void FrameRenderer::destroy_frame_sync() {
    material_overrides_.clear(); // the device is idle: no frame reads them
    if (device_ == nullptr || command_pool_ == VK_NULL_HANDLE) return;
    const VkDevice vk_device = device_->handle();
    for (VkSemaphore s : render_finished_) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    render_finished_.clear();
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        vkDestroySemaphore(vk_device, image_available_[i], nullptr);
        vkDestroyFence(vk_device, in_flight_[i], nullptr);
        image_available_[i] = VK_NULL_HANDLE;
        in_flight_[i] = VK_NULL_HANDLE;
    }
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

std::uint32_t FrameRenderer::wait_for_slot() {
    vkWaitForFences(device_->handle(), 1, &in_flight_[frame_], VK_TRUE, UINT64_MAX);
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
    VkResult acquire = vkAcquireNextImageKHR(vk_device, swapchain_->handle(), UINT64_MAX,
                                             image_available_[frame_], VK_NULL_HANDLE,
                                             &image_index_);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        needs_recreate_ = true;
        return Acquire::retry;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
        std::fprintf(stderr, "[fatal] vkAcquireNextImageKHR failed (%d)\n",
                     static_cast<int>(acquire));
        return Acquire::failed;
    }

    vkResetFences(vk_device, 1, &in_flight_[frame_]);

    VkCommandBuffer cmd = command_buffers_[frame_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

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
        capture = {frames_[frame_].viewport_image.handle(), swapchain_->format(), draw_extent_,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
#endif
    return capture;
}

bool FrameRenderer::record_screenshot_copy(VkCommandBuffer cmd) {
    // Screenshot capture on the final frame: the graph leaves the swapchain
    // image in PRESENT_SRC — round-trip it through TRANSFER_SRC and copy to
    // a host-visible buffer, written out as PPM after the queue idles.
    const VkExtent2D present_extent = swapchain_->extent();
    const VkDeviceSize shot_bytes =
        static_cast<VkDeviceSize>(present_extent.width) * present_extent.height * 4;
    if (screenshot_buffer_.handle() == VK_NULL_HANDLE) {
        auto buf = allocator_->create_buffer(shot_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_AUTO,
                                             VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
        if (buf) screenshot_buffer_ = std::move(*buf);
    }
    if (screenshot_buffer_.handle() == VK_NULL_HANDLE) return false;

    const VkImage shot_image = swapchain_->images()[image_index_];
    VkImageMemoryBarrier2 to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = shot_image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {present_extent.width, present_extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, shot_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           screenshot_buffer_.handle(), 1, &region);

    VkImageMemoryBarrier2 to_present = to_src;
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    // Chained onto by the render-finished signal (see k_present_signal_stage).
    to_present.dstStageMask = k_present_signal_stage;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    dep.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

void FrameRenderer::write_screenshot() const {
    const VkExtent2D present_extent = swapchain_->extent();
    const char* screenshot_path = screenshot_path_->c_str();
    const bool bgra = swapchain_->format() == VK_FORMAT_B8G8R8A8_SRGB ||
                      swapchain_->format() == VK_FORMAT_B8G8R8A8_UNORM;
    if (FILE* f = std::fopen(screenshot_path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", present_extent.width, present_extent.height);
        const auto* px = static_cast<const std::uint8_t*>(screenshot_buffer_.mapped());
        std::vector<std::uint8_t> row(static_cast<std::size_t>(present_extent.width) * 3);
        for (std::uint32_t y = 0; y < present_extent.height; ++y) {
            for (std::uint32_t x = 0; x < present_extent.width; ++x) {
                const std::uint8_t* p =
                    px + (static_cast<std::size_t>(y) * present_extent.width + x) * 4;
                row[x * 3 + 0] = bgra ? p[2] : p[0];
                row[x * 3 + 1] = p[1];
                row[x * 3 + 2] = bgra ? p[0] : p[2];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        std::fprintf(stderr, "[screenshot] wrote %s\n", screenshot_path);
    } else {
        std::fprintf(stderr, "[screenshot] could not open %s\n", screenshot_path);
    }
}

bool FrameRenderer::submit_and_present(bool screenshot_frame) {
    VkCommandBuffer cmd = command_buffers_[frame_];
    const bool screenshot_recorded =
        screenshot_frame && screenshot_path_.has_value() && record_screenshot_copy(cmd);

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
    vkQueueSubmit2(device_->graphics_queue(), 1, &submit, in_flight_[frame_]);

    VkSwapchainKHR sc = swapchain_->handle();
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_finished_[image_index_];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &image_index_;
    VkResult present_result = vkQueuePresentKHR(device_->present_queue(), &present);
    if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
        needs_recreate_ = true;
    } else if (present_result != VK_SUCCESS) {
        std::fprintf(stderr, "[fatal] vkQueuePresentKHR failed (%d)\n",
                     static_cast<int>(present_result));
        return false;
    }

    if (screenshot_recorded) {
        vkQueueWaitIdle(device_->graphics_queue());
        write_screenshot();
    }

    frame_ = (frame_ + 1) % k_frames_in_flight;
    return true;
}

} // namespace quat::app
