#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/core/deferred_release_queue.hpp"
#include "engine/core/error.hpp"
#include "engine/rhi/compute_pipeline.hpp"
#include "engine/rhi/graphics_pipeline.hpp"
#include "engine/rhi/managed_pipeline.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::rhi {

class Device;

// A SPIR-V shader module plus the reflection parsed from its sibling .json.
struct LoadedShader {
    VkShaderModule   module = VK_NULL_HANDLE;
    ShaderReflection reflection;
    // Declared format of each storage image (spirv_storage_image_formats);
    // pipeline creation checks the device supports them (Device::
    // check_storage_image_formats).
    std::vector<VkFormat> storage_image_formats;
    std::string           path; // normalised .spv path (the cache key); empty if not cached
};

// Outcome of PipelineCache::reload for one shader.
struct PipelineReloadReport {
    bool                     shader_in_use = false; // the cache had loaded this shader
    std::size_t              rebuilt = 0;           // pipelines replaced
    std::vector<std::string> errors;                // pipelines kept (with the reason)
};

// The most recent pipeline change a cache made: a pipeline created, or
// rebuilt by a shader reload. Logged when the GPU is lost (issue #110), since
// a freshly changed shader (e.g. a model-authored material) is the likely
// cause.
struct PipelineChange {
    std::string   what;      // e.g. "created graphics pipeline from <spv>"
    std::uint64_t frame = 0; // begin_frame()'s frame number at the time
};

// Loads and caches SPIR-V shader modules by source path, owns the Vulkan
// VkPipelineCache used when pipelines are created (Slice 2.5), and keeps the
// registry of ManagedPipelines by shader path so a recompiled shader can
// rebuild every pipeline built from it (ROADMAP R2). Move-only; owns all cached
// VkShaderModules. Main thread only.
class PipelineCache {
public:
    [[nodiscard]] static std::expected<PipelineCache, core::Error> create(const Device& device);

    PipelineCache() = default;
    ~PipelineCache();
    PipelineCache(PipelineCache&& other) noexcept;
    PipelineCache& operator=(PipelineCache&& other) noexcept;
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    [[nodiscard]] VkPipelineCache handle() const { return cache_; }

    // Loads `spv_path` (and its sibling reflection .json), creating and caching
    // the shader module. Repeated calls for the same path return the cached
    // entry. The returned pointer is stable for the cache's lifetime (a reload
    // updates the entry in place).
    [[nodiscard]] std::expected<const LoadedShader*, core::Error>
    load(const std::string& spv_path);

    // Create a pipeline registered for hot reload under its shaders' paths.
    // Same inputs as ComputePipeline/GraphicsPipeline::create; the spans are
    // copied. Shaders not loaded through this cache make a plain pipeline that
    // is never rebuilt.
    [[nodiscard]] std::expected<ManagedPipeline, core::Error>
    create_compute_pipeline(const ComputePipeline::CreateInfo& info);
    [[nodiscard]] std::expected<ManagedPipeline, core::Error>
    create_graphics_pipeline(const GraphicsPipeline::CreateInfo& info);

    // Re-reads `spv_path` + reflection after a recompile and rebuilds every live
    // pipeline using it. If the new reflection no longer fits any of those
    // pipelines' layouts (see check_reflection_compatible), nothing changes:
    // the old module and pipelines stay, and the misfits are listed in
    // `errors`. A pipeline whose re-creation fails keeps its old pipeline and
    // is listed too. Replaced pipelines and shader modules are destroyed by
    // begin_frame() once the frames in flight are done. Fails (changing
    // nothing) when the shader cannot be loaded.
    [[nodiscard]] std::expected<PipelineReloadReport, core::Error>
    reload(const std::string& spv_path);

    // Per-frame bookkeeping for deferred destruction: `frame_number` counts
    // submitted frames; call after waiting for the fence of the frame about to
    // be recorded. Objects replaced at frame N are destroyed at frame
    // N + frames_in_flight.
    void begin_frame(std::uint64_t frame_number);
    void set_frames_in_flight(std::uint32_t frames) { retired_.set_frames_in_flight(frames); }
    [[nodiscard]] std::size_t retired_count() const { return retired_.size(); }

    // The last pipeline created or rebuilt (none before the first).
    [[nodiscard]] const std::optional<PipelineChange>& last_change() const { return last_change_; }

    // Live registered pipelines using `spv_path` (diagnostics/tests).
    [[nodiscard]] std::size_t pipeline_count(const std::string& spv_path) const;

private:
    void destroy() noexcept;
    [[nodiscard]] std::expected<LoadedShader, core::Error> read_shader(const std::string& key) const;
    void register_slot(const std::shared_ptr<detail::PipelineSlot>& slot);
    // Whether `slot`'s layout fits its shaders with `key` replaced by `updated`.
    [[nodiscard]] std::expected<void, core::Error>
    check_compatible(const detail::PipelineSlot& slot, const std::string& key,
                     const ShaderReflection& updated) const;
    [[nodiscard]] std::expected<void, core::Error> rebuild(detail::PipelineSlot& slot);
    [[nodiscard]] const LoadedShader* find(const std::string& key) const;

    const Device*   device_ptr_ = nullptr;
    VkDevice        device_ = VK_NULL_HANDLE;
    VkPipelineCache cache_  = VK_NULL_HANDLE;
    std::unordered_map<std::string, LoadedShader> modules_;
    // Shader path -> pipelines built from it (expired entries are pruned lazily).
    std::unordered_map<std::string, std::vector<std::weak_ptr<detail::PipelineSlot>>> slots_;
    core::DeferredReleaseQueue retired_;
    std::uint64_t              frame_ = 0;
    std::optional<PipelineChange> last_change_;
};

} // namespace engine::rhi
