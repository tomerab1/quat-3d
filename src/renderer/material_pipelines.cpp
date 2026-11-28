#include "engine/renderer/material_pipelines.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <utility>

#include "engine/rhi/material_shader_names.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_reflection.hpp"

namespace engine::renderer {

MaterialPipelines::MaterialPipelines(rhi::PipelineCache& cache, std::string cooked_dir,
                                     std::string variant, const rhi::LoadedShader* baseline,
                                     Factory factory)
    : cache_(&cache), cooked_dir_(std::move(cooked_dir)), variant_(std::move(variant)),
      baseline_(baseline), factory_(std::move(factory)) {}

const rhi::ManagedPipeline* MaterialPipelines::find(std::string_view name) {
    if (cache_ == nullptr || !rhi::material_shaders::valid_name(name)) return nullptr;
    auto [it, inserted] = entries_.try_emplace(std::string(name));
    Entry& entry = it->second;
    if (entry.pipeline.handle() != VK_NULL_HANDLE) return &entry.pipeline;
    if (frame_ < entry.retry_frame) return nullptr;
    entry.retry_frame = frame_ + retry_interval;
    try_build(it->first, entry);
    return entry.pipeline.handle() != VK_NULL_HANDLE ? &entry.pipeline : nullptr;
}

void MaterialPipelines::try_build(const std::string& name, Entry& entry) {
    const std::string spv = rhi::material_shaders::cooked_spv_path(cooked_dir_, name, variant_);
    std::string error;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(spv, ec)) {
        error = "not compiled (" + spv + " missing)";
    } else if (auto shader = cache_->load(spv); !shader) {
        error = shader.error().message;
    } else {
        static const std::array<std::string, 2> entries{"vertex_main", "fragment_main"};
        if (baseline_ != nullptr) {
            if (auto fits = rhi::check_reflection_compatible(baseline_->reflection,
                                                             (*shader)->reflection, entries);
                !fits) {
                error = "does not fit the pass's pipeline layout: " + fits.error().message;
            }
        }
        if (error.empty()) {
            if (auto pipeline = factory_(**shader); !pipeline) {
                error = pipeline.error().message;
            } else {
                entry.pipeline = std::move(*pipeline);
            }
        }
    }
    if (error != entry.error && !error.empty()) {
        std::fprintf(stderr, "[shader] material '%s' (%s): %s\n", name.c_str(), variant_.c_str(),
                     error.c_str());
    }
    entry.error = std::move(error);
}

std::string_view MaterialPipelines::error(std::string_view name) const {
    const auto it = entries_.find(std::string(name));
    return it == entries_.end() ? std::string_view{} : std::string_view(it->second.error);
}

std::size_t MaterialPipelines::size() const {
    std::size_t n = 0;
    for (const auto& [name, entry] : entries_) {
        if (entry.pipeline.handle() != VK_NULL_HANDLE) ++n;
    }
    return n;
}

} // namespace engine::renderer
