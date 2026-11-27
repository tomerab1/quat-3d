#pragma once

// GPU particle wiring for the `game` executable (app layer, not engine): feeds
// the scene's ParticleEmitter components through a ParticleEmitterTable
// (slots, pool ranges, emission schedule — engine-side and unit-tested),
// streams their flipbooks in the background, and adds the ParticlePass to
// each frame's graph. The pass is created on first use (a scene without
// emitters pays nothing).

#include <chrono>
#include <cstdint>
#include <deque>
#include <expected>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>

#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/texture_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/particle_emitters.hpp"
#include "engine/renderer/particle_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/gltf_loader.hpp" // TextureData

namespace engine::rhi {
class Device;
class GpuAllocator;
class PipelineCache;
struct TransferContext;
} // namespace engine::rhi

namespace engine::scene {
struct CameraMatrices;
}

namespace quat::app {

// The frame inputs particles draw against.
struct ParticleFrameInputs {
    std::uint32_t                                slot = 0;   // frame in flight
    const engine::renderer::GBufferTargets*      gbuffer = nullptr;
    engine::rhi::ResourceHandle                  hdr{};
    const engine::renderer::ClusteredLights*     lights = nullptr;
    const engine::scene::CameraMatrices*         camera = nullptr; // unjittered
    glm::mat4                                    jittered_view_proj{1.0F};
    engine::renderer::DirectionalLightParams     sun{};
    VkExtent2D                                   extent{0, 0};
    float                                        sim_dt = 0.0F; // 0 = paused
};

class ParticleRuntime {
public:
    // References must outlive the runtime; `transfer` uploads flipbooks.
    // Flipbook paths resolve against `project_root`.
    ParticleRuntime(const engine::rhi::Device& device, engine::rhi::GpuAllocator& allocator,
                    engine::rhi::PipelineCache& cache, const engine::rhi::TransferContext& transfer,
                    std::string shader_dir, std::string project_root,
                    std::uint32_t frames_in_flight);

    // Once per frame, after the transparent pass: simulates and draws every
    // ParticleEmitter of `registry`.
    void add_to_graph(engine::rhi::RenderGraph& graph, const entt::registry& registry,
                      const ParticleFrameInputs& in);

    // Restarts every emitter (editor Play / Stop): ranges reinitialised,
    // emission schedules (and one-shot bursts) from the beginning.
    void reset_all() { table_.reset_all(); }

private:
    using Clock = std::chrono::steady_clock;
    using Decoded = std::expected<engine::scene::TextureData, engine::core::Error>;

    // A flipbook decodes on a worker thread and uploads on the main thread
    // once ready; until then (and after a failure) emitters draw the built-in
    // disc. Failures retry with a doubling back-off, or as soon as the file
    // changes.
    struct Flipbook {
        engine::asset::TextureAsset     texture;
        std::future<Decoded>            pending;
        bool                            ready = false;
        bool                            failed = false;
        Clock::time_point               retry_at{};
        Clock::time_point               next_poll{};
        std::chrono::seconds            backoff{1};
        std::filesystem::file_time_type stamp{};
    };

    [[nodiscard]] bool ensure_pass();
    [[nodiscard]] VkImageView flipbook(const std::string& path, Clock::time_point now);
    void start_load(const std::string& path, Flipbook& book);
    void warn_once(const std::string& message);

    const engine::rhi::Device&          device_;
    engine::rhi::GpuAllocator&          allocator_;
    engine::rhi::PipelineCache&         cache_;
    const engine::rhi::TransferContext& transfer_;
    std::string                         shader_dir_;
    std::string                         project_root_;
    std::uint32_t                       frames_in_flight_;

    std::optional<engine::renderer::ParticlePass> pass_;
    bool                                           create_failed_ = false;
    engine::renderer::ParticleEmitterTable         table_{engine::renderer::particle_default_pool};
    // This frame's sanitized settings (stable addresses for the inputs).
    std::deque<engine::renderer::ParticleEmitterSettings> frame_settings_;
    // Loaded flipbooks are kept for the runtime's life: frames in flight may
    // still sample one an emitter stopped using.
    std::unordered_map<std::string, Flipbook> flipbooks_;
    std::string                               last_warning_;
};

} // namespace quat::app
