#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace engine::rhi {

class PipelineCache;

namespace detail {
struct PipelineSlot;
}

// A pipeline created through PipelineCache that shader hot reload (ROADMAP R2)
// can rebuild in place. Passes hold one where they used to hold a
// ComputePipeline/GraphicsPipeline and read handle()/layout() when recording:
// after a reload they return the rebuilt pipeline (the layout is recreated with
// the same definition, so it stays compatible). Move-only; the pipeline is
// destroyed with the last owner (the cache defers destruction of replaced
// pipelines until no frame in flight uses them).
class ManagedPipeline {
public:
    ManagedPipeline() = default;
    ~ManagedPipeline();
    ManagedPipeline(ManagedPipeline&& other) noexcept;
    ManagedPipeline& operator=(ManagedPipeline&& other) noexcept;
    ManagedPipeline(const ManagedPipeline&) = delete;
    ManagedPipeline& operator=(const ManagedPipeline&) = delete;

    [[nodiscard]] VkPipeline handle() const;
    [[nodiscard]] VkPipelineLayout layout() const;
    // Incremented each time a reload replaces the pipeline (0 = original).
    [[nodiscard]] std::uint32_t generation() const;

private:
    friend class PipelineCache;
    explicit ManagedPipeline(std::shared_ptr<detail::PipelineSlot> slot);

    std::shared_ptr<detail::PipelineSlot> slot_;
};

} // namespace engine::rhi
