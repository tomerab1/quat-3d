#pragma once

// GPU particle wiring for the `game` executable (app layer, not engine):
// tracks the scene's ParticleEmitter components, gives each a stable slot and
// a range of the global particle pool, runs their emission schedule on the
// simulation clock, loads their flipbooks, and adds the ParticlePass to each
// frame's graph. The pass is created on first use (a scene without emitters
// pays nothing).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/asset/texture_asset.hpp"
#include "engine/renderer/light_cull_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/particle_pass.hpp"
#include "engine/renderer/particles.hpp"
#include "engine/rhi/render_graph.hpp"

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

private:
    struct Emitter {
        std::uint32_t                              slot = 0;
        std::uint32_t                              base = 0;
        std::uint32_t                              capacity = 0;
        engine::renderer::ParticleSpace            space = engine::renderer::ParticleSpace::world;
        engine::renderer::ParticleEmitterSettings  settings{}; // sanitized copy for this frame
        engine::renderer::ParticleSpawnState       spawn{};
        bool                                       fresh = true;
        bool                                       seen = false;
    };
    struct Flipbook {
        engine::asset::TextureAsset texture;
        bool                        failed = false;
    };

    [[nodiscard]] bool ensure_pass();
    // Allocates (or re-allocates after a capacity / space change) the
    // emitter's slot and pool range. False when the budget is exhausted.
    [[nodiscard]] bool place(entt::entity entity, Emitter& emitter);
    void release(Emitter& emitter);
    [[nodiscard]] VkImageView flipbook(const std::string& path);
    void warn_once(const std::string& message);

    const engine::rhi::Device&          device_;
    engine::rhi::GpuAllocator&          allocator_;
    engine::rhi::PipelineCache&         cache_;
    const engine::rhi::TransferContext& transfer_;
    std::string                         shader_dir_;
    std::string                         project_root_;
    std::uint32_t                       frames_in_flight_;

    std::optional<engine::renderer::ParticlePass>     pass_;
    bool                                               create_failed_ = false;
    engine::renderer::ParticlePoolAllocator            pool_{engine::renderer::particle_default_pool};
    std::array<bool, engine::renderer::particle_max_emitters> slot_used_{};
    std::unordered_map<entt::entity, Emitter>          emitters_;
    // Loaded flipbooks, kept for the runtime's life (frames in flight may
    // still sample one an emitter stopped using).
    std::unordered_map<std::string, Flipbook>          flipbooks_;
    std::string                                        last_warning_;
};

} // namespace quat::app
