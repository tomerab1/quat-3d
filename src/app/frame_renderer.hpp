#pragma once

// FrameRenderer — the `game` executable's per-frame rendering (app layer).
//
// Owns the per-frame-in-flight deferred chains (DeferredFrame: every pass
// instance, the transient image pool and the skinning buffers of one slot),
// the temporal state shared by both slots (TAA history, adapted exposure), the
// sky / environment (atmosphere LUTs, dynamic IBL probe), the command buffers
// and frame synchronisation, and the QUAT_SCREENSHOT capture.
//
// A frame, in order (main thread):
//   ensure_swapchain()     recreate after a resize / out-of-date present
//   wait_for_slot()        the slot's fence — its resources are free again
//   acquire_and_begin()    acquire the swapchain image, begin the command buffer
//   gather()               light, point lights, draw lists, skinning work and
//                          the shadow view from the ticked scene
//   record()               build, compile and execute the render graph
//   submit_and_present()   (screenshot copy,) submit, present, next slot
//
// Not movable: passes and the remote bridge hold pointers into it.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "app/remote_bridge.hpp" // CaptureSource
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/cloud_settings.hpp"
#include "engine/renderer/draw_batching.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/light_clusters.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/render_settings.hpp"
#include "engine/renderer/shadow_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/renderer/transparent_pass.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/render_graph.hpp"
#ifdef ENGINE_EDITOR
#include "engine/renderer/imgui_pass.hpp"
#endif

struct ImDrawData;

namespace engine::renderer {
class TerrainPass;
}
namespace engine::rhi {
class Device;
class PipelineCache;
class Swapchain;
} // namespace engine::rhi
namespace engine::scene {
class Scene;
struct CameraMatrices;
} // namespace engine::scene
namespace quat::app {

class GrassRuntime;

inline constexpr std::uint32_t k_frames_in_flight = 2;
// Stage the image-acquire semaphore is waited at; the swapchain image's first
// render-graph barrier is chained to it.
inline constexpr VkPipelineStageFlags2 k_acquire_wait_stage =
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

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

// Per-frame-in-flight bundle for the live deferred chain. Each frame slot owns
// its own pass instances (which hold per-frame descriptor buffers) and transient
// image pool, so building frame N+1's graph never stomps resources still in use
// by frame N. The main loop waits on the slot's fence before reusing it, which
// makes resetting the pool and rebuilding the descriptor buffers safe.
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

// The scene's render inputs for one frame, gathered after the simulation.
struct FrameScene {
    glm::mat4 jittered_vp{1.0F};     // TAA-jittered view-projection
    glm::mat4 jittered_inv_vp{1.0F};
    engine::renderer::DirectionalLightParams       light;
    std::vector<engine::renderer::GpuLight>        point_lights;
    std::vector<engine::renderer::DrawItem>        draws;             // opaque
    std::vector<engine::renderer::DrawItem>        transparent_draws; // back-to-front
    std::vector<engine::renderer::DrawItem>        shadow_draws;
    std::vector<engine::renderer::SkinDispatch>    dispatches;
    glm::mat4 light_view_proj{1.0F};
};

// Everything record() draws besides the gathered scene.
struct FrameInputs {
    const FrameScene*                    scene = nullptr;
    const entt::registry*                registry = nullptr; // InstancedMesh placements (R9.2)
    const engine::scene::CameraMatrices* cam = nullptr; // unjittered
    VkExtent2D                           draw_extent{0, 0};
    float                                dt = 0.0F;
    // Terrain tiles to draw into the GBuffer (null: none this frame).
    engine::renderer::TerrainPass* terrain = nullptr;
    glm::vec3                      terrain_anchor{0.0F};
    float                          terrain_snowline = 0.0F;
    // GPU grass around the camera (null: none); `sim_dt` drives its wind.
    GrassRuntime* grass = nullptr;
    float         sim_dt = 0.0F;
#ifdef ENGINE_EDITOR
    // Builds the editor UI (after the scene passes are declared) and returns
    // its draw data, drawn over the swapchain as the last pass.
    std::function<const ImDrawData*()> build_ui;
#endif
};

class FrameRenderer {
public:
    enum class Acquire { ok, retry, failed };

    FrameRenderer() = default;
    ~FrameRenderer();
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;
    FrameRenderer(FrameRenderer&&) = delete;
    FrameRenderer& operator=(FrameRenderer&&) = delete;

    // ---- Setup -------------------------------------------------------------
    // The frame slots' passes and the shared temporal state. `with_ui` adds
    // each slot's ImGui pass (editor builds with an active editor). Logs a
    // [fatal] line and returns false on failure.
    [[nodiscard]] bool create_passes(const engine::rhi::Device& device,
                                     engine::rhi::GpuAllocator& allocator,
                                     engine::rhi::PipelineCache& pipeline_cache,
                                     const engine::rhi::TransferContext& setup_transfer,
                                     const std::string& shader_dir,
                                     engine::rhi::Swapchain& swapchain, bool with_ui);
    // Every tunable renderer parameter (A8). Read by the passes each frame;
    // the editor's Renderer panel, the remote API and scene files edit it.
    [[nodiscard]] engine::renderer::RenderSettings& render_settings() { return settings_; }
    // Atmosphere LUTs + dynamic IBL probe, lit by the scene's sun. Optional:
    // failures are logged and fall back to the procedural sky / flat ambient.
    void create_environment(engine::scene::Scene& scene,
                            engine::rhi::PipelineCache& pipeline_cache,
                            const std::string& shader_dir);
    // Command buffers and frame synchronisation.
    void create_frame_sync();
    // QUAT_SCREENSHOT: the final frame is written to `path` as binary PPM.
    void set_screenshot_path(std::optional<std::string> path) {
        screenshot_path_ = std::move(path);
    }
    // Teardown, after the device went idle. Idempotent.
    void destroy_frame_sync();

    // ---- Per frame ---------------------------------------------------------
    void request_swapchain_recreate() { needs_recreate_ = true; }
    // Recreates the swapchain if requested. False on failure (logged).
    [[nodiscard]] bool ensure_swapchain(VkExtent2D pixel_extent);
    // Waits for the next slot's previous submission; returns the slot index.
    std::uint32_t wait_for_slot();
    [[nodiscard]] Acquire acquire_and_begin();
    // Accumulates the time that drives the clouds' wind drift.
    void advance_time(float dt) { cloud_time_s_ += dt; }

    [[nodiscard]] VkExtent2D present_extent() const;
    [[nodiscard]] FrameScene gather(engine::scene::Scene& scene,
                                    const engine::scene::CameraMatrices& cam,
                                    VkExtent2D draw_extent, std::uint64_t frame_number);
    // Builds and executes this frame's graph. False on a fatal pass error
    // (logged).
    [[nodiscard]] bool record(const FrameInputs& in);

    [[nodiscard]] VkCommandBuffer command_buffer() const { return command_buffers_[frame_]; }
    [[nodiscard]] std::uint32_t frame_slot() const { return frame_; }
    // The final scene image (no editor UI), for remote screenshots.
    [[nodiscard]] CaptureSource capture_source() const;
    // `screenshot_frame`: copy this frame to the QUAT_SCREENSHOT file. False
    // on a fatal present error (logged).
    [[nodiscard]] bool submit_and_present(bool screenshot_frame);

private:
    [[nodiscard]] engine::renderer::CloudSettings current_clouds() const;
    void recreate_render_finished();
    [[nodiscard]] bool record_screenshot_copy(VkCommandBuffer cmd);
    void write_screenshot() const;

    const engine::rhi::Device* device_ = nullptr;
    engine::rhi::GpuAllocator* allocator_ = nullptr;
    engine::rhi::Swapchain*    swapchain_ = nullptr;
    VkFormat                   ldr_format_ = VK_FORMAT_UNDEFINED;
    bool                       editor_active_ = false;
    engine::renderer::RenderSettings settings_;

    // Drives the clouds' wind drift (settings_.clouds.time_s is ignored). The
    // frame loop accumulates it always, like the animated sky in other engines
    // — not gated on play mode.
    float cloud_time_s_ = 0.0F;

    // Temporal state shared by both slots (audit A2 / A11): one TAA history and
    // one adapted luminance, each advanced exactly once per frame. Declared
    // before `frames_` so it outlives every pass that references it.
    std::shared_ptr<engine::renderer::TaaHistory>    taa_history_;
    std::shared_ptr<engine::renderer::ExposureState> exposure_state_;
    std::array<DeferredFrame, k_frames_in_flight>    frames_;

    std::optional<engine::renderer::AtmospherePass> atmosphere_;
    std::optional<engine::renderer::DynamicIbl>     dynamic_ibl_;

    // image_available + in-flight fence are per frame-in-flight; render_finished
    // is per swapchain image (it is signalled by the submit that targets that
    // image and waited on by its present).
    VkCommandPool                                   command_pool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, k_frames_in_flight> command_buffers_{};
    std::array<VkSemaphore, k_frames_in_flight>     image_available_{};
    std::array<VkFence, k_frames_in_flight>         in_flight_{};
    std::vector<VkSemaphore>                        render_finished_;
    bool                                            needs_recreate_ = false;

    std::uint32_t frame_ = 0;       // current frame-in-flight slot
    std::uint32_t image_index_ = 0; // acquired swapchain image
    VkExtent2D    draw_extent_{0, 0};

    std::optional<std::string> screenshot_path_;
    engine::rhi::GpuBuffer     screenshot_buffer_;
};

} // namespace quat::app
