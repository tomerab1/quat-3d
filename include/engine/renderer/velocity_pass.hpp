#pragma once

// VelocityPass — completes the GBuffer's velocity target for the background
// (ROADMAP R11; contract in velocity.hpp).
//
// Declare after every opaque GBuffer writer (mesh, terrain, grass): a
// fullscreen triangle at depth 1, depth-tested EQUAL against the GBuffer depth
// (read-only), writes the camera-only velocity of the far plane wherever no
// geometry was drawn. Stateless apart from its pipeline (no descriptors), so
// one instance serves every frame in flight.

#include <expected>
#include <string>

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>

#include "engine/core/error.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/render_graph.hpp"

namespace engine::rhi {
class Device;
class PipelineCache;
}

namespace engine::renderer {

class VelocityPass {
public:
    [[nodiscard]] static std::expected<VelocityPass, core::Error>
    create(const rhi::Device& device, rhi::PipelineCache& cache,
           const std::string& cooked_shader_dir);

    VelocityPass() = default;
    VelocityPass(VelocityPass&&) noexcept = default;
    VelocityPass& operator=(VelocityPass&&) noexcept = default;
    VelocityPass(const VelocityPass&) = delete;
    VelocityPass& operator=(const VelocityPass&) = delete;

    // `view_proj` is this frame's UNjittered view-projection; the previous one
    // and the jitter come from `gbuffer.motion`.
    void add_background_to_graph(rhi::RenderGraph& graph, const GBufferTargets& gbuffer,
                                 VkExtent2D extent, const glm::mat4& view_proj) const;

private:
    rhi::DescriptorSetLayout empty_layout_; // see create()
    rhi::ManagedPipeline     pipeline_;
};

} // namespace engine::renderer
