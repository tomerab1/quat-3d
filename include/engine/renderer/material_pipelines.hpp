#pragma once

// MaterialPipelines — the pipelines a pass builds for authored material
// shaders (ROADMAP A6), by shader name.
//
// A material shader is compiled (offline by CMake, or at runtime by shader hot
// reload) into one SPIR-V program per pass variant,
// cooked/shaders/materials/<name>.<variant>.spv, with exactly the pipeline
// layout of the pass's built-in program. The pass asks find(name) while
// recording a frame: the first request loads the program through the
// PipelineCache, checks its reflection against the built-in program's
// (rhi::check_reflection_compatible — a program that would not fit the layout
// is never handed to the driver) and builds the pipeline with the pass's
// factory. Pipelines are ManagedPipelines, so a later recompile swaps the
// program in place (PipelineCache::reload). A name that is not usable yet —
// not compiled, or not layout-compatible — yields null (the pass draws with
// its built-in program) and is retried every `retry_interval` frames, which
// is how a material created at runtime appears without any notification.

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine/core/error.hpp"
#include "engine/rhi/managed_pipeline.hpp"

namespace engine::rhi {
class PipelineCache;
struct LoadedShader;
} // namespace engine::rhi

namespace engine::renderer {

class MaterialPipelines {
public:
    // Builds the pass's pipeline from a material's loaded program.
    using Factory =
        std::function<std::expected<rhi::ManagedPipeline, core::Error>(const rhi::LoadedShader&)>;

    static constexpr std::uint32_t retry_interval = 30; // frames

    MaterialPipelines() = default;
    // `baseline` is the pass's built-in program (stable PipelineCache entry);
    // `variant` names the cooked program (rhi::material_shaders::variants).
    MaterialPipelines(rhi::PipelineCache& cache, std::string cooked_dir, std::string variant,
                      const rhi::LoadedShader* baseline, Factory factory);

    // Once per recorded frame, before find().
    void begin_frame() { ++frame_; }

    // The pipeline for material shader `name`, or null when it cannot be used
    // (yet). Names that are not valid material shader names are always null.
    [[nodiscard]] const rhi::ManagedPipeline* find(std::string_view name);

    // Why `name` has no pipeline (empty when it has one, or was never asked).
    [[nodiscard]] std::string_view error(std::string_view name) const;

    // Pipelines built so far.
    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        rhi::ManagedPipeline pipeline;
        std::uint64_t        retry_frame = 0; // next frame a missing pipeline is tried
        std::string          error;
    };

    void try_build(const std::string& name, Entry& entry);

    rhi::PipelineCache*         cache_ = nullptr; // non-owning
    std::string                 cooked_dir_;
    std::string                 variant_;
    const rhi::LoadedShader*    baseline_ = nullptr;
    Factory                     factory_;
    std::unordered_map<std::string, Entry> entries_;
    std::uint64_t               frame_ = 0;
};

} // namespace engine::renderer
