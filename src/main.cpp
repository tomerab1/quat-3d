// quat-3d engine entry point.
//
// Phase 2, Slice 2.5: a hard-coded triangle is drawn via the render graph. Each
// frame builds a RenderGraph that imports the acquired swapchain image and runs
// a single GraphicsPass (dynamic rendering) that clears to black and draws the
// triangle from mesh.slang. The graph inserts the layout barriers and the
// trailing transition to PRESENT_SRC. Two frames in flight.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "app/audio_runtime.hpp"
#include "app/demo_scenes.hpp"
#include "app/fly_camera.hpp"
#include "app/nav_bake.hpp"
#include "app/remote_bridge.hpp"
#include "app/world_runtime.hpp"
#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/asset/mesh_asset.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/session.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/terrain/streamer.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"

#ifdef ENGINE_EDITOR
#include "editor/scene_snapshot.hpp"
#include "engine/editor/editor.hpp"
#include "engine/renderer/imgui_pass.hpp"
#endif
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/rhi/swapchain.hpp"

namespace {

constexpr int  k_window_width  = 1280;
constexpr int  k_window_height = 720;
constexpr char k_window_title[] = "quat-3d";
constexpr std::uint32_t k_frames_in_flight = 2;
// Stage the image-acquire semaphore is waited at; the swapchain image's first
// render-graph barrier is chained to it.
constexpr VkPipelineStageFlags2 k_acquire_wait_stage =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

#ifndef NDEBUG
constexpr bool k_enable_validation = true;
#else
constexpr bool k_enable_validation = false;
#endif

VkExtent2D window_pixel_extent(SDL_Window* window) {
    int w = 0;
    int h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return VkExtent2D{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}


// Per-frame-in-flight bundle for the live deferred chain. Each frame slot owns
// its own pass instances (which hold per-frame descriptor buffers) and transient
// image pool, so building frame N+1's graph never stomps resources still in use
// by frame N. The main loop waits on the slot's fence before reusing it, which
// makes resetting the pool and rebuilding the descriptor buffers safe.
// Per-skinned-entity GPU buffers for one frame slot: the joint matrices (host
// visible, re-uploaded each frame) and the skinned vertex output (device local,
// written by the skinning pass and read by the GBuffer as a vertex buffer).
struct SkinnedEntityGpu {
    engine::rhi::GpuBuffer joints;
    engine::rhi::GpuBuffer skinned_verts;
    VkDeviceAddress        joints_addr = 0;
    VkDeviceAddress        skinned_addr = 0;
    std::uint32_t          vertex_count = 0;
    std::uint32_t          joint_count = 0;
};

struct DeferredFrame {
    engine::renderer::MeshPass       mesh;
    engine::renderer::LightCullPass  light_cull;
    engine::renderer::LightingPass   lighting;
    engine::renderer::TonemapPass    tonemap;
    engine::renderer::SkinningPass   skinning;
    engine::renderer::ShadowPass     shadow;
    engine::renderer::TransparentPass transparent;
    engine::renderer::BloomPass      bloom;
    engine::renderer::ExposurePass   exposure;
    engine::renderer::TaaPass        taa;
#ifdef ENGINE_EDITOR
    engine::renderer::ImGuiPass      imgui;
    // Offscreen scene target for the editor's Viewport panel: TAA resolves the
    // frame here (instead of the swapchain) and the UI samples it. Recreated
    // when the panel is resized.
    engine::rhi::GpuImage            viewport_image;
    engine::rhi::ImageView           viewport_view;
    VkExtent2D                       viewport_extent{0, 0};
#endif
    engine::rhi::TransientImagePool  pool;
    // Skinning resources, lazily created per skinned entity (keyed by entity).
    std::unordered_map<entt::entity, SkinnedEntityGpu> skin_gpu;
};

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

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        k_window_title, k_window_width, k_window_height,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
    if (window == nullptr) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    Uint32 sdl_ext_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&sdl_ext_count);
    if (sdl_extensions == nullptr) {
        SDL_Log("SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto device_result = engine::rhi::Device::create({
        .instance_extensions = {sdl_extensions, sdl_ext_count},
        .enable_validation = k_enable_validation,
        .application_name = k_window_title,
    });
    if (!device_result) {
        std::fprintf(stderr, "[fatal] device creation failed: %s\n",
                     device_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Device& device = *device_result;
    const VkDevice vk_device = device.handle();

    auto allocator_result = engine::rhi::GpuAllocator::create(device);
    if (!allocator_result) {
        std::fprintf(stderr, "[fatal] allocator creation failed: %s\n",
                     allocator_result.error().message.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::GpuAllocator& allocator = *allocator_result;


    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, device.instance(), nullptr, &surface)) {
        std::fprintf(stderr, "[fatal] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    auto swapchain_result =
        engine::rhi::Swapchain::create(device, surface, window_pixel_extent(window));
    if (!swapchain_result) {
        std::fprintf(stderr, "[fatal] swapchain creation failed: %s\n",
                     swapchain_result.error().message.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    engine::rhi::Swapchain& swapchain = *swapchain_result;

    // ---- Deferred renderer + demo scene ------------------------------------
    auto fatal = [&](const char* what, const std::string& detail) {
        std::fprintf(stderr, "[fatal] %s: %s\n", what, detail.c_str());
        vkDestroySurfaceKHR(device.instance(), surface, nullptr);
        SDL_DestroyWindow(window);
        SDL_Quit();
    };

    auto pipeline_cache = engine::rhi::PipelineCache::create(device);
    if (!pipeline_cache) {
        fatal("pipeline cache creation failed", pipeline_cache.error().message);
        return 1;
    }

    const std::string shader_dir = QUAT_COOKED_SHADER_DIR;
    const VkFormat swap_format = swapchain.format();
    // The swapchain is usually sRGB, which cannot be a storage image (TAA writes
    // one) and would double-encode if the tonemap shader's explicit sRGB output
    // were hardware-encoded again. So tonemap + post-process work in the matching
    // UNORM format (the shader does the sRGB encode), then a size-compatible copy
    // moves the bits into the sRGB swapchain.
    const VkFormat ldr_format = [swap_format] {
        switch (swap_format) {
            case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
            case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
            default: return swap_format;
        }
    }();

    // One-time transfer context for setup uploads (demo mesh/material + each
    // pass's fallback resources). Destroyed once setup completes.
    VkCommandPool setup_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        // Graphics family: mip-generation blits (vkCmdBlitImage) are only legal
        // on a graphics-capable queue.
        info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &info, nullptr, &setup_pool);
    }
    const engine::rhi::TransferContext setup_transfer{vk_device, setup_pool,
                                                      device.graphics_queue(),
                                                      device.max_sampler_anisotropy()};

#ifdef ENGINE_EDITOR
    // Editor shell (ImGui context + SDL3 input backend). Created before the
    // frame slots: each slot's ImGuiPass uploads the font atlas of this
    // context. QUAT_NO_UI=1 disables it (deterministic headless renders).
    engine::editor::EditorLayer editor;
    engine::editor::RendererSettings render_settings;
    std::string pending_instantiate; // set by the asset browser, consumed below
    std::string pending_save;        // scene file requests from the File menu
    std::string pending_load;
    // Play mode: snapshot taken on Play, restored on Stop; the play session
    // owns its own physics world (demo modes keep theirs).
    bool editor_play = false;
    bool play_use_scene_camera = false; // menu-bar "Game camera" checkbox
    bool editor_play_prev = false;
    std::optional<engine::editor::SceneSnapshot> play_snapshot;
    const bool editor_enabled = std::getenv("QUAT_NO_UI") == nullptr;
    if (editor_enabled) {
        if (auto e = engine::editor::EditorLayer::create(
                window, std::string(QUAT_PROJECT_ROOT) + "/assets/fonts/Roboto-Medium.ttf");
            e) {
            editor = std::move(*e);
        } else {
            std::fprintf(stderr, "[warn] editor disabled: %s\n", e.error().message.c_str());
        }
    }
#endif

    // Cloud parameters for the lighting background + IBL bake: the editor's
    // Renderer panel edits them; headless builds use the defaults. The frame
    // loop accumulates cloud_time_s to drive the wind drift (always, like the
    // animated sky in other engines — not gated on play mode).
    float cloud_time_s = 0.0F;
    const auto current_clouds = [&]() -> engine::renderer::CloudSettings {
        engine::renderer::CloudSettings c{};
#ifdef ENGINE_EDITOR
        c = render_settings.clouds;
#endif
        c.time_s = cloud_time_s;
        return c;
    };

    // Temporal state shared by both slots (audit A2 / A11): one TAA history and
    // one adapted luminance, each advanced exactly once per frame. Declared
    // before `frames` so it outlives every pass that references it.
    const auto taa_history =
        std::make_shared<engine::renderer::TaaHistory>(device, allocator, k_frames_in_flight);
    auto exposure_state = engine::renderer::ExposureState::create(device, allocator);
    // Per-frame-in-flight deferred chains (GBuffer -> lighting -> tonemap). Each
    // slot owns its passes' descriptor buffers and a transient image pool, so
    // building one frame's graph never disturbs the other in-flight frame.
    std::array<DeferredFrame, k_frames_in_flight> frames;
    bool frames_ok = exposure_state.has_value();
    if (!exposure_state) {
        std::fprintf(stderr, "[fatal] exposure state creation failed: %s\n",
                     exposure_state.error().message.c_str());
    }
    for (auto& f : frames) {
        if (!frames_ok) break;
        auto mesh = engine::renderer::MeshPass::create(device, allocator, *pipeline_cache,
                                                       setup_transfer, shader_dir);
        auto light = engine::renderer::LightingPass::create(device, allocator, *pipeline_cache,
                                                            shader_dir);
        auto tone = engine::renderer::TonemapPass::create(device, allocator, *pipeline_cache,
                                                          shader_dir, ldr_format);
        auto skin = engine::renderer::SkinningPass::create(device, allocator, *pipeline_cache,
                                                           shader_dir);
        auto shadow = engine::renderer::ShadowPass::create(device, allocator, *pipeline_cache,
                                                           shader_dir);
        auto transp = engine::renderer::TransparentPass::create(device, allocator, *pipeline_cache,
                                                                setup_transfer, shader_dir);
        auto bloom = engine::renderer::BloomPass::create(device, allocator, *pipeline_cache,
                                                         shader_dir);
        auto expo = engine::renderer::ExposurePass::create(device, allocator, *pipeline_cache,
                                                           shader_dir, *exposure_state);
        auto taa = engine::renderer::TaaPass::create(device, allocator, *pipeline_cache, shader_dir,
                                                     ldr_format, taa_history);
#ifdef ENGINE_EDITOR
        if (editor.active()) {
            auto ui = engine::renderer::ImGuiPass::create(device, allocator, *pipeline_cache,
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
        auto cull = engine::renderer::LightCullPass::create(device, allocator, *pipeline_cache,
                                                            shader_dir);
        if (!cull) {
            frames_ok = false;
            std::fprintf(stderr, "[fatal] light cull pass creation failed: %s\n",
                         cull.error().message.c_str());
            break;
        }
        f.light_cull = std::move(*cull);
    }

    // Scene assets live in an AssetManager so MeshRenderer handles keep them
    // alive. QUAT_GLTF=<path> instantiates a real glTF scene graph; otherwise a
    // spinning demo cube stands in.
    engine::asset::AssetManager assets;
    engine::scene::Scene scene;
    quat::app::WorldRuntime world;
    const bool scene_ok = world.build_scene(scene, assets, allocator, setup_transfer);
    const bool physics_mode = world.physics_mode();
    const bool character_mode = world.character_mode();

    vkDestroyCommandPool(vk_device, setup_pool, nullptr);

    if (!frames_ok || !scene_ok) {
        fatal("deferred renderer setup failed", "see [fatal] lines above");
        return 1;
    }

    quat::app::add_default_lighting(scene, !world.from_gltf());
    // Physically-based atmosphere (Phase 11.1): static transmittance/multi-
    // scatter LUTs now, sky-view LUT refreshed whenever the sun moves. The
    // procedural sky remains the fallback if creation fails.
    std::optional<engine::renderer::AtmospherePass> atmosphere;
    {
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
        const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : dlv) {
            sun_to = -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
            break;
        }
        if (auto atmos = engine::renderer::AtmospherePass::create(device, allocator,
                                                                  *pipeline_cache, shader_dir);
            atmos) {
            atmosphere = std::move(*atmos);
            if (auto r = atmosphere->ensure_skyview(sun_to); !r) {
                std::fprintf(stderr, "[warn] sky-view LUT failed: %s\n",
                             r.error().message.c_str());
                atmosphere.reset();
            } else {
                std::fprintf(stderr, "[info] atmosphere LUTs ready\n");
            }
        } else {
            std::fprintf(stderr, "[warn] atmosphere create failed: %s\n",
                         atmos.error().message.c_str());
        }
    }
    const auto atmos_skyview = [&]() {
        return atmosphere ? atmosphere->skyview_view() : VK_NULL_HANDLE;
    };
    const auto atmos_transmittance = [&]() {
        return atmosphere ? atmosphere->transmittance_view() : VK_NULL_HANDLE;
    };

    // Image-based lighting: a dynamic environment probe (11.3). create() runs
    // one blocking full cycle so ambient/reflections are complete from frame
    // one; afterwards the probe re-renders incrementally, one compute step per
    // frame inside the render graph, so the IBL tracks the moving sun and
    // drifting clouds with no rebake hitches. Falls back to flat ambient on
    // failure.
    std::optional<engine::renderer::DynamicIbl> dynamic_ibl;
    {
        glm::vec3 sun_to = glm::normalize(glm::vec3(0.4F, 1.0F, 0.3F));
        const auto dlv = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : dlv) {
            sun_to = -glm::normalize(dlv.get<const engine::scene::DirectionalLight>(e).direction);
            break;
        }
        engine::renderer::IblCycleParams initial{};
        initial.sun_to = sun_to;
        initial.skyview = atmos_skyview();
        initial.transmittance = atmos_transmittance();
        initial.clouds = current_clouds();
        if (auto dyn = engine::renderer::DynamicIbl::create(device, allocator, *pipeline_cache,
                                                            shader_dir, initial);
            dyn) {
            dynamic_ibl = std::move(*dyn);
            std::fprintf(stderr, "[info] dynamic IBL probe ready\n");
        } else {
            std::fprintf(stderr, "[warn] dynamic IBL create failed: %s\n",
                         dyn.error().message.c_str());
        }
    }

    // Terrain, navmesh, networking (QUAT_HOST / QUAT_JOIN) and QUAT_TERRAIN.
    world.init(device, allocator, *pipeline_cache, shader_dir, scene);
#ifdef ENGINE_EDITOR
    std::optional<engine::editor::EditorContext::GroundClick> ground_click; // 13.4
#endif

    // Free-fly view camera, seeded by framing the scene (see fly_camera.hpp).
    quat::app::FlyCamera camera = quat::app::FlyCamera::create(scene, physics_mode);
    // ---- Per-frame command + sync resources --------------------------------
    VkCommandPool command_pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool_info.queueFamilyIndex = device.queue_families().graphics;
        vkCreateCommandPool(vk_device, &pool_info, nullptr, &command_pool);
    }

    std::array<VkCommandBuffer, k_frames_in_flight> command_buffers{};
    {
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = command_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = k_frames_in_flight;
        vkAllocateCommandBuffers(vk_device, &alloc, command_buffers.data());
    }

    auto make_semaphore = [vk_device] {
        VkSemaphore s = VK_NULL_HANDLE;
        VkSemaphoreCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(vk_device, &info, nullptr, &s);
        return s;
    };

    // image_available + in-flight fence are per frame-in-flight; render_finished
    // is per swapchain image (it is signalled by the submit that targets that
    // image and waited on by its present).
    std::array<VkSemaphore, k_frames_in_flight> image_available{};
    std::array<VkFence, k_frames_in_flight> in_flight{};
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        image_available[i] = make_semaphore();
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk_device, &fence_info, nullptr, &in_flight[i]);
    }

    std::vector<VkSemaphore> render_finished(swapchain.image_count(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_finished) {
        s = make_semaphore();
    }

    auto recreate_render_finished = [&] {
        for (VkSemaphore s : render_finished) {
            vkDestroySemaphore(vk_device, s, nullptr);
        }
        render_finished.assign(swapchain.image_count(), VK_NULL_HANDLE);
        for (VkSemaphore& s : render_finished) {
            s = make_semaphore();
        }
    };

    // ---- Main loop ---------------------------------------------------------
    // QUAT_MAX_FRAMES, when set, auto-quits after that many presented frames.
    // Used for headless smoke tests so clean teardown can be verified.
    std::uint64_t max_frames = 0;
    if (const char* env = std::getenv("QUAT_MAX_FRAMES"); env != nullptr) {
        max_frames = std::strtoull(env, nullptr, 10);
    }
    // QUAT_SCREENSHOT, combined with QUAT_MAX_FRAMES, writes the final presented
    // frame to the given path as binary PPM — headless visual verification.
    const char* screenshot_path = std::getenv("QUAT_SCREENSHOT");
    engine::rhi::GpuBuffer screenshot_buffer;
    bool screenshot_recorded = false;

    std::uint64_t presented = 0;
    double        frame_time_sum = 0.0;     // avg-frame-time report at exit
    std::uint64_t frame_time_samples = 0;
    std::uint32_t frame = 0;
    bool running = true;
    bool needs_recreate = false;
    std::uint64_t last_ticks = SDL_GetTicksNS();

    // Remote control (ROADMAP A2/A3): QUAT_REMOTE=<port> opens the loopback
    // JSON-RPC endpoint the quat-mcp bridge talks to (QUAT_REMOTE_TOKEN
    // requires session.hello). The bridge also resolves Primitive components
    // every frame, endpoint or not. See src/app/remote_bridge.hpp.
    quat::app::RemoteBridgeTargets remote_targets;
    remote_targets.device = &device;
    remote_targets.allocator = &allocator;
    remote_targets.assets = &assets;
    remote_targets.scene = &scene;
    remote_targets.frame_counter = &presented;
    remote_targets.cam_pos = &camera.pos;
    remote_targets.cam_yaw = &camera.yaw;
    remote_targets.cam_pitch = &camera.pitch;
    remote_targets.cam_fov_y = &camera.fov_y;
    world.fill_remote_targets(remote_targets);
#ifdef ENGINE_EDITOR
    remote_targets.editor = &editor;
    remote_targets.render_settings = &render_settings;
    remote_targets.play = world.has_demo_physics() ? nullptr : &editor_play; // demo modes own physics
#endif
    remote_targets.project_root = QUAT_PROJECT_ROOT;
    quat::app::RemoteBridge remote_bridge(std::move(remote_targets), k_frames_in_flight);

    world.init_audio(QUAT_PROJECT_ROOT);

    while (running) {
        camera.begin_input(); // per-frame accumulated mouse-look delta

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
#ifdef ENGINE_EDITOR
            editor.process_event(event);
#endif
            switch (event.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_ESCAPE) running = false;
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                needs_recreate = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_MOTION: {
                bool accept_clicks = true;
#ifdef ENGINE_EDITOR
                // Camera input belongs to the Viewport panel; clicks anywhere
                // else are the UI's.
                accept_clicks = !(editor.active() && !editor.viewport_hovered());
#endif
                camera.handle_event(event, window, accept_clicks);
                break;
            }
            default:
                break;
            }
        }
        if (!running) break;

        // Remote-control requests (and primitive resolution) run here: after
        // input, before play-mode transitions, scene file requests and the
        // scene tick.
        remote_bridge.poll();

#ifdef ENGINE_EDITOR
        editor.begin_frame();

        // Play/Stop transition: Play snapshots the scene and brings up a
        // dedicated physics world; Stop tears the world down and restores the
        // snapshot (exact entity ids — selection and hierarchy stay valid).
        if (editor_play != editor_play_prev) {
            if (editor_play) {
                play_snapshot = engine::editor::capture_scene(scene);
                // Unity convention: a Collider without a RigidBody simulates as
                // a static body (kinematic on nav agents, none on characters).
                // Added AFTER the snapshot, so Stop removes it.
                (void)engine::scene::promote_collider_only_bodies(scene.registry());
                if (auto started = world.begin_play(scene); started) {
                    std::fprintf(stderr, "[editor] play (%zu colliders, %zu bodies)\n",
                                 scene.registry().view<engine::scene::Collider>().size(),
                                 scene.registry().view<engine::scene::RigidBody>().size());
                } else {
                    std::fprintf(stderr, "[editor] play failed: %s\n",
                                 started.error().message.c_str());
                    editor_play = false;
                    play_snapshot.reset();
                }
            } else {
                world.end_play();
                if (play_snapshot) {
                    engine::editor::restore_scene(scene, *play_snapshot);
                    play_snapshot.reset();
                }
                std::fprintf(stderr, "[editor] stop\n");
            }
            editor_play_prev = editor_play;
        }

        // Scene save/load requested from the File menu. Saving is pure CPU;
        // loading clears the scene and needs an upload context for asset
        // re-resolution, then the loop's cached entities are rebound.
        if (!pending_save.empty()) {
            const std::string path = std::exchange(pending_save, {});
            if (auto r = engine::scene::save_scene(scene, path); r) {
                std::fprintf(stderr, "[editor] saved scene: %s\n", path.c_str());
            } else {
                std::fprintf(stderr, "[editor] save failed: %s\n", r.error().message.c_str());
            }
        }
        if (!pending_load.empty() && !editor_play) {
            const std::string path = std::exchange(pending_load, {});
            VkCommandPool load_pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext load_transfer{
                    vk_device, load_pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto r = engine::scene::load_scene(scene, path, allocator, load_transfer,
                                                       assets);
                    r) {
                    std::fprintf(stderr, "[editor] loaded scene: %s\n", path.c_str());
                    world.invalidate_navmesh("scene loaded");
                    // Rebind the loop's cached entities to the loaded scene.
                    // (The view camera is editor state — nothing to rebind.)
                    world.clear_spin_entity();
                    editor.set_selected(entt::null);
                    // The dynamic IBL probe picks up the loaded scene's sun
                    // within one refresh cycle — no explicit rebake needed.
                } else {
                    std::fprintf(stderr, "[editor] load failed: %s\n",
                                 r.error().message.c_str());
                }
                vkDestroyCommandPool(vk_device, load_pool, nullptr);
            }
        }

        // glTF instantiate requested from the asset browser (double-click or
        // viewport drop): blocking load through a transient upload context on
        // the graphics queue (mip blits), like the startup path.
        if (!pending_instantiate.empty()) {
            const std::string path = std::exchange(pending_instantiate, {});
            VkCommandPool load_pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_info.queueFamilyIndex = device.queue_families().graphics;
            if (vkCreateCommandPool(vk_device, &pool_info, nullptr, &load_pool) == VK_SUCCESS) {
                const engine::rhi::TransferContext load_transfer{
                    vk_device, load_pool, device.graphics_queue(),
                    device.max_sampler_anisotropy()};
                if (auto roots = engine::scene::GltfLoader::instantiate(
                        path, allocator, load_transfer, assets, scene);
                    roots) {
                    // Place the new roots in front of the camera (instead of at
                    // their authored origin, which may be out of view) and
                    // select the first so the gizmo picks it up immediately.
                    const glm::vec3 spawn = camera.pos + camera.forward() * 5.0F;
                    for (const entt::entity root : *roots) {
                        auto& t = scene.registry().get<engine::scene::Transform>(root);
                        t.local = glm::translate(glm::mat4(1.0F), spawn) * t.local;
                    }
                    if (!roots->empty()) editor.set_selected(roots->front());
                    std::fprintf(stderr, "[editor] instantiated %s (%zu roots)\n", path.c_str(),
                                 roots->size());
                } else {
                    std::fprintf(stderr, "[editor] instantiate failed: %s\n",
                                 roots.error().message.c_str());
                }
                vkDestroyCommandPool(vk_device, load_pool, nullptr);
            }
        }
#endif

        const VkExtent2D pixel_extent = window_pixel_extent(window);
        if (pixel_extent.width == 0 || pixel_extent.height == 0) {
            SDL_Delay(10); // minimized — nothing to draw
            continue;
        }

        if (needs_recreate) {
            if (auto r = swapchain.recreate(pixel_extent); !r) {
                std::fprintf(stderr, "[fatal] swapchain recreate failed: %s\n",
                             r.error().message.c_str());
                break;
            }
            recreate_render_finished();
            needs_recreate = false;
        }

        vkWaitForFences(vk_device, 1, &in_flight[frame], VK_TRUE, UINT64_MAX);
        remote_bridge.on_frame_complete(frame); // the slot's screenshot readback landed

        std::uint32_t image_index = 0;
        VkResult acquire = vkAcquireNextImageKHR(
            vk_device, swapchain.handle(), UINT64_MAX, image_available[frame],
            VK_NULL_HANDLE, &image_index);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            needs_recreate = true;
            continue;
        }
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            std::fprintf(stderr, "[fatal] vkAcquireNextImageKHR failed (%d)\n",
                         static_cast<int>(acquire));
            break;
        }

        vkResetFences(vk_device, 1, &in_flight[frame]);

        VkCommandBuffer cmd = command_buffers[frame];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        // Build this frame's deferred graph into the slot's own resources and
        // render the scene to the acquired swapchain image. The slot's fence was
        // waited above, so resetting its transient pool and rebuilding the passes'
        // descriptor buffers does not race the other in-flight frame.
        DeferredFrame& fr = frames[frame];
        fr.pool.reset();

        // The scene renders at `draw_extent`; the swapchain presents at
        // `present_extent`. They differ only in editor mode, where the scene
        // fills the Viewport panel (sized by last frame's UI) instead of the
        // whole window.
        const VkExtent2D present_extent = swapchain.extent();
        VkExtent2D draw_extent = present_extent;
#ifdef ENGINE_EDITOR
        if (editor.active() && editor.viewport_width() > 0 && editor.viewport_height() > 0) {
            draw_extent.width = std::clamp(editor.viewport_width(), 16U, 4096U);
            draw_extent.height = std::clamp(editor.viewport_height(), 16U, 4096U);
        }
#endif
        const float aspect =
            draw_extent.height == 0
                ? 1.0F
                : static_cast<float>(draw_extent.width) / static_cast<float>(draw_extent.height);

        // Free-fly camera: right-drag looks, WASD moves, Q/E down/up, Shift = fast.
        const std::uint64_t now_ticks = SDL_GetTicksNS();
        const float dt = static_cast<float>(now_ticks - last_ticks) * 1e-9F;
        last_ticks = now_ticks;
        cloud_time_s += dt;

        camera.apply_look();
        quat::app::FlyInputGate gate;
#ifdef ENGINE_EDITOR
        // Editor mode: WASD flies only during a right-drag (W/E/R switch the
        // gizmo otherwise); the ARROW keys fly whenever the cursor is over the
        // viewport (they conflict with nothing). Text input freezes both.
        const bool typing = editor.wants_text_input();
        gate.wasd = !typing && (!editor.active() || camera.looking() || character_mode);
        gate.arrows =
            !typing && (!editor.active() || camera.looking() || editor.viewport_hovered());
#endif

        if (character_mode) {
            world.drive_character(scene, camera.character_wish(gate), dt);
        } else {
            camera.fly(scene, gate, dt);
        }

        const quat::app::PlayStep* play = nullptr;
#ifdef ENGINE_EDITOR
        quat::app::PlayStep play_step;
        if (editor_play) {
            play_step.use_scene_camera = play_use_scene_camera;
            play_step.fly_camera_pos = camera.pos;
            play = &play_step;
        }
#endif
        world.simulate(scene, presented, dt, play);

        // Third-person follow camera, placed after the character has moved.
        if (character_mode) camera.follow(scene, world.character_entity());
#ifdef ENGINE_EDITOR
        // The editor's fly camera is the default view in BOTH modes — Play
        // must never take movement away from the user (old scene files also
        // carry stale "camera" entities that would hijack the view). The
        // menu-bar "Game camera" checkbox opts the play view into the scene's
        // active Camera entity (a camera parented to a moving object follows
        // it); it falls back to the fly camera when none is active.
        const bool through_scene_camera =
            editor_play && play_use_scene_camera &&
            engine::scene::find_active_camera(scene.registry()) != entt::null;
        const engine::scene::CameraMatrices cam =
            through_scene_camera
                ? engine::scene::camera_system(scene.registry(), aspect)
                : camera.matrices(aspect);
        world.update_audio(scene, cam.view, editor_play, dt);
#else
        const engine::scene::CameraMatrices cam =
            engine::scene::camera_system(scene.registry(), aspect);
        world.update_audio(scene, cam.view, /*live=*/true, dt);
#endif

        // TAA: jitter the projection sub-pixel each frame (Halton). The whole
        // frame renders with the jittered view-proj; the resolve accumulates
        // into the shared history, reprojecting with the UNjittered
        // cam.view_proj (audit A14).
        const glm::vec2 jitter =
            engine::renderer::taa_jitter(presented, draw_extent.width, draw_extent.height);
        glm::mat4 jitter_mat(1.0F);
        jitter_mat[3][0] = jitter.x;
        jitter_mat[3][1] = jitter.y;
        const glm::mat4 jittered_vp = jitter_mat * cam.view_proj;
        const glm::mat4 jittered_inv_vp = glm::inverse(jittered_vp);

        engine::renderer::DirectionalLightParams light;
        const auto light_view = scene.registry().view<const engine::scene::DirectionalLight>();
        for (const entt::entity e : light_view) {
            const auto& dl = light_view.get<const engine::scene::DirectionalLight>(e);
            light.direction = glm::vec4(-glm::normalize(dl.direction), 0.0F);
            light.color = glm::vec4(dl.color, dl.intensity);
            break;
        }

        // Gather point lights from the ECS (world position from Transform); the
        // light-cull compute pass bins them into view clusters on the GPU.
        std::vector<engine::renderer::GpuLight> point_lights;
        for (auto [e, t, pl] :
             scene.registry()
                 .view<const engine::scene::Transform, const engine::scene::PointLight>()
                 .each()) {
            point_lights.push_back(engine::renderer::make_point_light(
                glm::vec3(t.world[3]), pl.radius, pl.color, pl.intensity));
        }

        // Build the draw list, collecting per-skinned-entity skinning work. A
        // skinned entity uploads its joint matrices into this frame slot's buffer
        // and gets a SkinDispatch; its draw points at the skinned output buffer.
        std::vector<engine::renderer::DrawItem> draws;
        std::vector<engine::renderer::DrawItem> transparent_draws;
        std::vector<engine::renderer::SkinDispatch> dispatches;
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
                    std::memcpy(g->joints.mapped(), sm->joint_matrices.data(),
                                sm->joint_matrices.size() * sizeof(glm::mat4));
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
                    dispatches.push_back(d);
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

        // Sort transparent draws back-to-front (farthest from the camera first).
        std::sort(transparent_draws.begin(), transparent_draws.end(),
                  [&](const engine::renderer::DrawItem& a, const engine::renderer::DrawItem& b) {
                      const float da = glm::distance(glm::vec3(a.model[3]), cam.position);
                      const float db = glm::distance(glm::vec3(b.model[3]), cam.position);
                      return da > db;
                  });

        // Shadow casters: all opaques plus transmissive glass (solid surfaces —
        // they block the sun like opaque geometry; alpha-blended items do not cast).
        std::vector<engine::renderer::DrawItem> shadow_draws = draws;
        for (const engine::renderer::DrawItem& d : transparent_draws) {
            if (d.material != nullptr &&
                (d.material->params.flags & engine::asset::material_transmission) != 0) {
                shadow_draws.push_back(d);
            }
        }

        // Directional shadow: an orthographic light view fit to a fixed box
        // around the scene (the demos all sit near the origin).
        const glm::vec3 sun_dir = -glm::normalize(glm::vec3(light.direction)); // travel direction
        const glm::vec3 shadow_center(0.0F, 1.5F, 0.0F);
        constexpr float shadow_radius = 14.0F;
        const glm::vec3 light_eye = shadow_center - sun_dir * (shadow_radius * 1.5F);
        const glm::mat4 shadow_view =
            glm::lookAt(light_eye, shadow_center, glm::vec3(0.0F, 1.0F, 0.0F));
        const glm::mat4 shadow_proj = glm::ortho(-shadow_radius, shadow_radius, -shadow_radius,
                                                 shadow_radius, 0.1F, shadow_radius * 3.5F);
        const glm::mat4 light_view_proj = shadow_proj * shadow_view;

        world.update_terrain(scene, cam.position);

#ifdef ENGINE_EDITOR
        // Ctrl/Shift+click in the viewport (13.4): resolve the ray against the
        // terrain (bisected raymarch) or the ground plane, then command the
        // selected entity: Ctrl = walk here now, Shift = append a patrol point.
        if (ground_click) {
            const auto click = *ground_click;
            ground_click.reset();
            const auto& height_at = world.ground_height(); // shared sampler
            std::optional<glm::vec3> hit;
            float prev_t = 0.0F;
            for (float t = 2.0F; t < 2000.0F && !hit; t += 2.0F) {
                const glm::vec3 pos = click.origin + click.dir * t;
                const auto h = height_at(pos.x, pos.z);
                if (h && pos.y <= *h) {
                    // Bisect between the last point above and this one below.
                    float lo = prev_t;
                    float hi = t;
                    for (int i = 0; i < 16; ++i) {
                        const float mid = 0.5F * (lo + hi);
                        const glm::vec3 mp = click.origin + click.dir * mid;
                        const auto mh = height_at(mp.x, mp.z);
                        (mh && mp.y <= *mh ? hi : lo) = mid;
                    }
                    const glm::vec3 hp = click.origin + click.dir * hi;
                    hit = glm::vec3(hp.x, height_at(hp.x, hp.z).value_or(hp.y), hp.z);
                }
                prev_t = t;
            }
            if (!hit && click.dir.y < -1e-4F) { // no terrain: ground plane
                const float t = (world.terrain_anchor().y - click.origin.y) / click.dir.y;
                if (t > 0.0F) hit = click.origin + click.dir * t;
            }
            const entt::entity sel = editor.selected();
            if (hit && scene.registry().valid(sel) &&
                scene.registry().any_of<engine::scene::Transform>(sel)) {
                auto& agent = scene.registry().get_or_emplace<engine::scene::NavAgent>(sel);
                if (click.add_waypoint) {
                    auto& route =
                        scene.registry().get_or_emplace<engine::scene::PatrolRoute>(sel);
                    route.points.push_back(*hit);
                    std::fprintf(stderr, "[nav] waypoint %zu at (%.1f, %.1f, %.1f)\n",
                                 route.points.size(), hit->x, hit->y, hit->z);
                } else {
                    engine::scene::nav_agent_command(agent, *hit);
                    std::fprintf(stderr, "[nav] go to (%.1f, %.1f, %.1f)\n", hit->x, hit->y,
                                 hit->z);
                }
            }
        }
#endif

        world.service_navmesh_request(scene);

        engine::rhi::RenderGraph graph(fr.pool);

        // Sky + environment upkeep, declared FIRST so their trailing barriers
        // precede every consumer this frame (the graph keeps declaration order
        // for independent passes). The sky-view LUT recomputes in-graph when
        // the sun moved (no pipeline stall on sun drags); the dynamic IBL
        // probe advances one step per frame and flips when a cycle completes.
        const glm::vec3 sun_to = glm::vec3(light.direction);
        if (atmosphere) {
            if (auto r = atmosphere->add_skyview_update_to_graph(graph, sun_to); !r) {
                std::fprintf(stderr, "[warn] sky-view update: %s\n", r.error().message.c_str());
            }
        }
        if (dynamic_ibl) {
            engine::renderer::IblCycleParams cycle{};
            cycle.sun_to = sun_to;
            cycle.skyview = atmos_skyview();
            cycle.transmittance = atmos_transmittance();
            cycle.clouds = current_clouds();
            if (auto r = dynamic_ibl->add_step_to_graph(graph, cycle); !r) {
                std::fprintf(stderr, "[warn] ibl step: %s\n", r.error().message.c_str());
            }
        }
        const engine::renderer::IblViewSet ibl_views =
            dynamic_ibl ? dynamic_ibl->views() : engine::renderer::IblViewSet{};

        // R9.1: draws sharing mesh + material collapse into instanced draws.
        const engine::renderer::InstancedDrawList shadow_batches =
            engine::renderer::batch_draws(shadow_draws);
        const engine::renderer::InstancedDrawList opaque_batches =
            engine::renderer::batch_draws(draws);
        auto shadow = fr.shadow.add_to_graph(graph, light_view_proj, shadow_batches);
        if (!shadow) {
            std::fprintf(stderr, "[fatal] shadow pass: %s\n", shadow.error().message.c_str());
            break;
        }
        auto gbuffer = fr.mesh.add_to_graph(graph, draw_extent, jittered_vp, opaque_batches);
        if (!gbuffer) {
            std::fprintf(stderr, "[fatal] gbuffer pass: %s\n", gbuffer.error().message.c_str());
            break;
        }
        if (world.terrain_drawable()) {
            world.terrain_pass().add_to_graph(graph, *gbuffer, draw_extent, jittered_vp,
                                              cam.position, world.terrain_anchor(),
                                              world.terrain_snowline());
        }
        // Clustered light culling (R10.1). A camera the clusters cannot
        // represent (non-perspective / no finite far plane) renders unlit by
        // point lights rather than stopping the frame loop.
        engine::renderer::ClusteredLights clustered{};
        if (auto c = fr.light_cull.add_to_graph(graph, cam.view, cam.projection, point_lights)) {
            clustered = *c;
        } else {
            std::fprintf(stderr, "[warn] light cull: %s\n", c.error().message.c_str());
        }
        auto hdr = fr.lighting.add_to_graph(graph, *gbuffer, draw_extent, light, jittered_inv_vp,
                                            cam.position, *shadow, light_view_proj, clustered,
                                            /*enable_sky=*/true, ibl_views, atmos_skyview(),
                                            atmos_transmittance(), current_clouds());
        if (!hdr) {
            std::fprintf(stderr, "[fatal] lighting pass: %s\n", hdr.error().message.c_str());
            break;
        }
        // Forward-blend transparent geometry over the lit HDR, depth-tested
        // against the opaque GBuffer depth.
        if (!transparent_draws.empty()) {
            if (auto r = fr.transparent.add_to_graph(graph, *hdr, gbuffer->depth, draw_extent,
                                                     jittered_vp, light, cam.position,
                                                     transparent_draws, ibl_views, clustered);
                !r) {
                std::fprintf(stderr, "[fatal] transparent pass: %s\n", r.error().message.c_str());
                break;
            }
        }
        // Bloom: blur the bright parts of the HDR and add them back before tonemap.
        engine::renderer::BloomParams bloom_params{};
#ifdef ENGINE_EDITOR
        bloom_params.threshold = render_settings.bloom_threshold;
        bloom_params.knee = render_settings.bloom_knee;
        bloom_params.intensity = render_settings.bloom_intensity;
        bloom_params.radius = render_settings.bloom_radius;
#endif
        if (auto r = fr.bloom.add_to_graph(graph, *hdr, draw_extent, bloom_params); !r) {
            std::fprintf(stderr, "[fatal] bloom pass: %s\n", r.error().message.c_str());
            break;
        }
        // Auto-exposure: measure the (post-bloom) HDR luminance and adapt over time.
        if (auto r = fr.exposure.add_to_graph(graph, *hdr, draw_extent, dt); !r) {
            std::fprintf(stderr, "[fatal] exposure pass: %s\n", r.error().message.c_str());
            break;
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
        if (editor.active()) {
            if (!ensure_viewport_target(fr, device, allocator, swapchain.format(), draw_extent)) {
                std::fprintf(stderr, "[fatal] viewport target creation failed\n");
                break;
            }
            viewport_handle = graph.import_image(
                "viewport", fr.viewport_image.handle(), fr.viewport_view.handle(),
                swapchain.format(), draw_extent, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            scene_target = viewport_handle;
        }
#endif
        // Tonemap to an intermediate LDR image; TAA resolves it into the target.
        const engine::rhi::ResourceHandle ldr_current = graph.create_transient_image(
            "ldr_current", ldr_format, draw_extent,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (auto r = fr.tonemap.add_to_graph(graph, *hdr, ldr_current, draw_extent,
                                             fr.exposure.exposure_buffer_address());
            !r) {
            std::fprintf(stderr, "[fatal] tonemap pass: %s\n", r.error().message.c_str());
            break;
        }
        // Temporal anti-aliasing: resolve the jittered frame against history.
        if (auto r = fr.taa.add_to_graph(graph, ldr_current, gbuffer->depth, scene_target,
                                         draw_extent, cam.view_proj);
            !r) {
            std::fprintf(stderr, "[fatal] taa pass: %s\n", r.error().message.c_str());
            break;
        }
#ifdef ENGINE_EDITOR
        // Editor UI: build the dockspace + panels (the Viewport panel samples
        // the offscreen scene image), then draw the UI into the swapchain as
        // the last pass. A null/empty draw list (editor disabled) adds no pass.
        {
            engine::editor::EditorContext ui_ctx;
            ui_ctx.frame_ms = dt * 1000.0F;
            ui_ctx.draw_count = static_cast<int>(draws.size() + transparent_draws.size());
            ui_ctx.entity_count = static_cast<int>(
                scene.registry().view<engine::scene::Transform>().size());
            ui_ctx.scene = &scene;
            ui_ctx.renderer = &render_settings;
            ui_ctx.project_root = QUAT_PROJECT_ROOT;
            ui_ctx.instantiate_request = &pending_instantiate;
            ui_ctx.save_request = &pending_save;
            ui_ctx.load_request = &pending_load;
            // Demo modes own their physics world — hide the Play button there.
            ui_ctx.play_mode = world.has_demo_physics() ? nullptr : &editor_play;
            ui_ctx.play_use_scene_camera =
                world.has_demo_physics() ? nullptr : &play_use_scene_camera;
            ui_ctx.build_navmesh_request = &world.navmesh().request;
            ui_ctx.ground_click = &ground_click;
            ui_ctx.nav_edges =
                world.navmesh().mesh.valid() ? &world.navmesh().mesh.debug_edges() : nullptr;
            ui_ctx.nav_status = &world.navmesh().status;
            ui_ctx.view_proj = cam.view_proj; // unjittered, for overlays
            ui_ctx.view = cam.view;
            ui_ctx.proj_no_flip = cam.projection;
            ui_ctx.proj_no_flip[1][1] *= -1.0F; // undo the Vulkan Y flip for ImGuizmo
            editor.build_ui(ui_ctx);
            if (auto r = fr.imgui.add_to_graph(graph, backbuffer, present_extent,
                                               editor.end_frame(), viewport_handle,
                                               /*clear=*/editor.active());
                !r) {
                std::fprintf(stderr, "[fatal] imgui pass: %s\n", r.error().message.c_str());
                break;
            }
        }
#endif
        if (auto compiled = graph.compile(); !compiled) {
            std::fprintf(stderr, "[fatal] render graph compile failed: %s\n",
                         compiled.error().message.c_str());
            break;
        }
        // Skin (compute) before the graph; record() adds the compute -> vertex
        // fetch barrier so the GBuffer sees the skinned vertices.
        if (!dispatches.empty()) {
            if (auto rs = fr.skinning.record(cmd, dispatches); !rs) {
                std::fprintf(stderr, "[fatal] skinning pass: %s\n", rs.error().message.c_str());
                break;
            }
        }
        graph.execute(cmd);

        // Remote screenshots copy the final scene image (no editor UI).
        quat::app::CaptureSource capture{swapchain.images()[image_index], swapchain.format(),
                                         present_extent, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
#ifdef ENGINE_EDITOR
        if (editor.active()) {
            capture = {fr.viewport_image.handle(), swapchain.format(), draw_extent,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
#endif
        remote_bridge.record_capture(cmd, frame, capture);

        // Screenshot capture on the final frame: the graph leaves the swapchain
        // image in PRESENT_SRC — round-trip it through TRANSFER_SRC and copy to
        // a host-visible buffer, written out as PPM after the queue idles.
        screenshot_recorded = false;
        if (screenshot_path != nullptr && max_frames != 0 && presented + 1 >= max_frames) {
            const VkDeviceSize shot_bytes =
                static_cast<VkDeviceSize>(present_extent.width) * present_extent.height * 4;
            if (screenshot_buffer.handle() == VK_NULL_HANDLE) {
                auto buf = allocator.create_buffer(shot_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   VMA_MEMORY_USAGE_AUTO,
                                                   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT |
                                                       VMA_ALLOCATION_CREATE_MAPPED_BIT);
                if (buf) screenshot_buffer = std::move(*buf);
            }
            if (screenshot_buffer.handle() != VK_NULL_HANDLE) {
                const VkImage shot_image = swapchain.images()[image_index];
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
                                       screenshot_buffer.handle(), 1, &region);

                VkImageMemoryBarrier2 to_present = to_src;
                to_present.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                to_present.dstAccessMask = 0;
                to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                dep.pImageMemoryBarriers = &to_present;
                vkCmdPipelineBarrier2(cmd, &dep);
                screenshot_recorded = true;
            }
        }

        vkEndCommandBuffer(cmd);

        VkSemaphoreSubmitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        wait.semaphore = image_available[frame];
        wait.stageMask = k_acquire_wait_stage;

        VkSemaphoreSubmitInfo signal{};
        signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signal.semaphore = render_finished[image_index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

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
        vkQueueSubmit2(device.graphics_queue(), 1, &submit, in_flight[frame]);

        VkSwapchainKHR sc = swapchain.handle();
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished[image_index];
        present.swapchainCount = 1;
        present.pSwapchains = &sc;
        present.pImageIndices = &image_index;
        VkResult present_result = vkQueuePresentKHR(device.graphics_queue(), &present);
        if (present_result == VK_ERROR_OUT_OF_DATE_KHR || present_result == VK_SUBOPTIMAL_KHR) {
            needs_recreate = true;
        } else if (present_result != VK_SUCCESS) {
            std::fprintf(stderr, "[fatal] vkQueuePresentKHR failed (%d)\n",
                         static_cast<int>(present_result));
            break;
        }

        if (screenshot_recorded) {
            vkQueueWaitIdle(device.graphics_queue());
            const bool bgra = swapchain.format() == VK_FORMAT_B8G8R8A8_SRGB ||
                              swapchain.format() == VK_FORMAT_B8G8R8A8_UNORM;
            if (FILE* f = std::fopen(screenshot_path, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", present_extent.width, present_extent.height);
                const auto* px = static_cast<const std::uint8_t*>(screenshot_buffer.mapped());
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

        // Average frame time over the run, skipping the first frame (it carries
        // one-time pipeline/descriptor warmup). Reported at exit.
        if (presented > 0) {
            frame_time_sum += dt;
            frame_time_samples += 1;
        }

        frame = (frame + 1) % k_frames_in_flight;
        // Advances every frame (audit A1): drives TAA jitter, the demo cube,
        // frame stats and engine.info — not only QUAT_MAX_FRAMES runs.
        ++presented;
        if (max_frames != 0 && presented >= max_frames) {
            running = false;
        }
    }

    if (frame_time_samples > 0) {
        const double avg_ms = frame_time_sum / static_cast<double>(frame_time_samples) * 1000.0;
        std::fprintf(stderr, "[stats] avg frame %.2f ms (%.1f fps) over %llu frames\n", avg_ms,
                     avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0,
                     static_cast<unsigned long long>(frame_time_samples));
    }

    // ---- Teardown ----------------------------------------------------------
    vkDeviceWaitIdle(vk_device);
    world.shutdown(); // waits for a running terrain job, frees its upload pool
    for (VkSemaphore s : render_finished) {
        vkDestroySemaphore(vk_device, s, nullptr);
    }
    for (std::uint32_t i = 0; i < k_frames_in_flight; ++i) {
        vkDestroySemaphore(vk_device, image_available[i], nullptr);
        vkDestroyFence(vk_device, in_flight[i], nullptr);
    }
    vkDestroyCommandPool(vk_device, command_pool, nullptr);
    swapchain = engine::rhi::Swapchain{};     // destroy swapchain before surface
    vkDestroySurfaceKHR(device.instance(), surface, nullptr);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
