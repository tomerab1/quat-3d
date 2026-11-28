#include "engine/rhi/pipeline_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "engine/core/utf8_path.hpp"
#include "engine/rhi/device.hpp"

namespace engine::rhi {

namespace {

// A shader for the log: its cache path, or a placeholder for one created
// outside the cache.
[[nodiscard]] std::string shader_label(const LoadedShader& shader) {
    return shader.path.empty() ? std::string("<uncached shader>") : shader.path;
}

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::expected<std::vector<std::uint32_t>, core::Error>
read_spirv(const std::string& path) {
    std::ifstream file(core::path_from_utf8(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return fail("cannot open SPIR-V file: " + path);
    }
    const std::streamsize bytes = file.tellg();
    if (bytes <= 0 || bytes % 4 != 0) {
        return fail("invalid SPIR-V size: " + path);
    }
    std::vector<std::uint32_t> words(static_cast<std::size_t>(bytes) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), bytes);
    if (!file) {
        return fail("failed reading SPIR-V file: " + path);
    }
    return words;
}

[[nodiscard]] std::expected<std::string, core::Error> read_text(const std::string& path) {
    std::ifstream file(core::path_from_utf8(path), std::ios::binary);
    if (!file) {
        return fail("cannot open reflection file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string reflection_path_for(const std::string& spv_path) {
    constexpr std::string_view ext = ".spv";
    if (spv_path.size() >= ext.size() &&
        spv_path.compare(spv_path.size() - ext.size(), ext.size(), ext) == 0) {
        return spv_path.substr(0, spv_path.size() - ext.size()) + ".json";
    }
    return spv_path + ".json";
}

// Cache key for a shader path: lexically normalised, so "a/./b.spv" and
// "a/b.spv" name the same entry (passes and the hot-reload service build their
// paths independently).
[[nodiscard]] std::string shader_key(const std::string& spv_path) {
    return core::path_to_generic_utf8(core::path_from_utf8(spv_path).lexically_normal());
}

} // namespace

namespace detail {

// One registered pipeline: the live pipeline plus the recipe to rebuild it.
// Heap-allocated and never moved, so the CreateInfo spans/strings below point
// into this object's own members.
struct PipelineSlot {
    struct Shader {
        std::string              path;     // cache key
        std::vector<std::string> entries;  // entry points the pipeline uses
        ShaderReflection         baseline; // reflection the layout was built for
    };

    bool                is_compute = true;
    ComputePipeline     compute;
    GraphicsPipeline    graphics;
    std::uint32_t       generation = 0;
    std::vector<Shader> shaders;

    // Recipe storage.
    std::string                                    entry_a; // compute / vertex entry
    std::string                                    entry_b; // fragment entry
    std::string                                    vertex_path;
    std::string                                    fragment_path;
    std::vector<VkDescriptorSetLayout>             set_layouts;
    std::vector<VkPushConstantRange>               push_constants;
    std::vector<VkFormat>                          color_formats;
    std::vector<VkPipelineColorBlendAttachmentState> blend_attachments;
    std::vector<VkVertexInputBindingDescription>   vertex_bindings;
    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    ComputePipeline::CreateInfo                    compute_info{};  // shader set per build
    GraphicsPipeline::CreateInfo                   graphics_info{}; // shaders set per build

    PipelineSlot() = default;
    PipelineSlot(const PipelineSlot&) = delete;
    PipelineSlot& operator=(const PipelineSlot&) = delete;

    [[nodiscard]] VkPipeline handle() const {
        return is_compute ? compute.handle() : graphics.handle();
    }
    [[nodiscard]] VkPipelineLayout layout() const {
        return is_compute ? compute.layout() : graphics.layout();
    }
};

} // namespace detail

// ---------------------------------------------------------------------------
// ManagedPipeline
// ---------------------------------------------------------------------------

ManagedPipeline::ManagedPipeline(std::shared_ptr<detail::PipelineSlot> slot)
    : slot_(std::move(slot)) {}
ManagedPipeline::~ManagedPipeline() = default;
ManagedPipeline::ManagedPipeline(ManagedPipeline&& other) noexcept = default;
ManagedPipeline& ManagedPipeline::operator=(ManagedPipeline&& other) noexcept = default;

VkPipeline ManagedPipeline::handle() const { return slot_ ? slot_->handle() : VK_NULL_HANDLE; }

VkPipelineLayout ManagedPipeline::layout() const {
    return slot_ ? slot_->layout() : VK_NULL_HANDLE;
}

std::uint32_t ManagedPipeline::generation() const { return slot_ ? slot_->generation : 0; }

// ---------------------------------------------------------------------------
// PipelineCache
// ---------------------------------------------------------------------------

std::expected<PipelineCache, core::Error> PipelineCache::create(const Device& device) {
    PipelineCache out;
    out.device_ptr_ = &device;
    out.device_ = device.handle();

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (VkResult r = vkCreatePipelineCache(out.device_, &info, nullptr, &out.cache_);
        r != VK_SUCCESS) {
        return fail("vkCreatePipelineCache failed");
    }
    return out;
}

std::expected<LoadedShader, core::Error> PipelineCache::read_shader(const std::string& key) const {
    auto words = read_spirv(key);
    if (!words) return std::unexpected(words.error());
    const std::string reflection_path = reflection_path_for(key);
    auto reflection_text = read_text(reflection_path);
    if (!reflection_text) return std::unexpected(reflection_text.error());
    auto reflection = parse_reflection_json(*reflection_text);
    if (!reflection) {
        return fail(reflection.error().message + " (" + reflection_path + ")");
    }

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = words->size() * sizeof(std::uint32_t);
    module_info.pCode = words->data();

    LoadedShader out;
    if (VkResult r = vkCreateShaderModule(device_, &module_info, nullptr, &out.module);
        r != VK_SUCCESS) {
        return fail("vkCreateShaderModule failed for " + key);
    }
    out.reflection = std::move(*reflection);
    out.storage_image_formats = spirv_storage_image_formats(*words);
    out.path = key;
    return out;
}

std::expected<const LoadedShader*, core::Error>
PipelineCache::load(const std::string& spv_path) {
    const std::string key = shader_key(spv_path);
    if (auto it = modules_.find(key); it != modules_.end()) {
        return &it->second;
    }
    auto shader = read_shader(key);
    if (!shader) return std::unexpected(shader.error());
    auto [it, inserted] = modules_.emplace(key, std::move(*shader));
    return &it->second;
}

const LoadedShader* PipelineCache::find(const std::string& key) const {
    const auto it = modules_.find(key);
    return it == modules_.end() ? nullptr : &it->second;
}

void PipelineCache::register_slot(const std::shared_ptr<detail::PipelineSlot>& slot) {
    for (const auto& shader : slot->shaders) {
        auto& list = slots_[shader.path];
        std::erase_if(list, [](const auto& w) { return w.expired(); });
        list.push_back(slot);
    }
}

std::expected<ManagedPipeline, core::Error>
PipelineCache::create_compute_pipeline(const ComputePipeline::CreateInfo& info) {
    if (device_ptr_ == nullptr) return fail("PipelineCache: no device");
    if (info.shader == nullptr) return fail("PipelineCache: compute shader missing");
    auto pipeline = ComputePipeline::create(*device_ptr_, cache_, info);
    if (!pipeline) return std::unexpected(pipeline.error());

    auto slot = std::make_shared<detail::PipelineSlot>();
    slot->is_compute = true;
    slot->compute = std::move(*pipeline);
    slot->entry_a = info.entry != nullptr ? info.entry : "";
    slot->set_layouts.assign(info.set_layouts.begin(), info.set_layouts.end());
    slot->push_constants.assign(info.push_constants.begin(), info.push_constants.end());
    slot->compute_info.entry = slot->entry_a.c_str();
    slot->compute_info.set_layouts = slot->set_layouts;
    slot->compute_info.push_constants = slot->push_constants;

    if (find(info.shader->path) == info.shader) { // loaded through this cache
        slot->shaders.push_back({info.shader->path, {slot->entry_a}, info.shader->reflection});
        register_slot(slot);
    }
    last_change_ = PipelineChange{"created compute pipeline from " + shader_label(*info.shader),
                                  frame_};
    return ManagedPipeline(std::move(slot));
}

std::expected<ManagedPipeline, core::Error>
PipelineCache::create_graphics_pipeline(const GraphicsPipeline::CreateInfo& info) {
    if (device_ptr_ == nullptr) return fail("PipelineCache: no device");
    if (info.vertex == nullptr || info.fragment == nullptr) {
        return fail("PipelineCache: vertex/fragment shader missing");
    }
    auto pipeline = GraphicsPipeline::create(*device_ptr_, cache_, info);
    if (!pipeline) return std::unexpected(pipeline.error());

    auto slot = std::make_shared<detail::PipelineSlot>();
    slot->is_compute = false;
    slot->graphics = std::move(*pipeline);
    slot->entry_a = info.vertex_entry != nullptr ? info.vertex_entry : "";
    slot->entry_b = info.fragment_entry != nullptr ? info.fragment_entry : "";
    slot->set_layouts.assign(info.set_layouts.begin(), info.set_layouts.end());
    // Pin the push-constant ranges the layout was built with: a rebuild must not
    // re-derive them from (possibly shrunk) new reflection.
    if (info.push_constants.empty()) {
        slot->push_constants = reflected_push_constant_ranges(*info.vertex, *info.fragment);
    } else {
        slot->push_constants.assign(info.push_constants.begin(), info.push_constants.end());
    }
    slot->color_formats.assign(info.color_formats.begin(), info.color_formats.end());
    slot->blend_attachments.assign(info.blend_attachments.begin(), info.blend_attachments.end());
    slot->vertex_bindings.assign(info.vertex_bindings.begin(), info.vertex_bindings.end());
    slot->vertex_attributes.assign(info.vertex_attributes.begin(), info.vertex_attributes.end());

    GraphicsPipeline::CreateInfo& g = slot->graphics_info;
    g = info;
    g.vertex = nullptr;
    g.fragment = nullptr;
    g.vertex_entry = slot->entry_a.c_str();
    g.fragment_entry = slot->entry_b.c_str();
    g.color_formats = slot->color_formats;
    g.blend_attachments = slot->blend_attachments;
    g.vertex_bindings = slot->vertex_bindings;
    g.vertex_attributes = slot->vertex_attributes;
    g.set_layouts = slot->set_layouts;
    g.push_constants = slot->push_constants;

    const bool cached =
        find(info.vertex->path) == info.vertex && find(info.fragment->path) == info.fragment;
    if (cached) {
        slot->vertex_path = info.vertex->path;
        slot->fragment_path = info.fragment->path;
        if (info.vertex == info.fragment) {
            slot->shaders.push_back(
                {info.vertex->path, {slot->entry_a, slot->entry_b}, info.vertex->reflection});
        } else {
            slot->shaders.push_back({info.vertex->path, {slot->entry_a}, info.vertex->reflection});
            slot->shaders.push_back(
                {info.fragment->path, {slot->entry_b}, info.fragment->reflection});
        }
        register_slot(slot);
    }
    std::string from = shader_label(*info.vertex);
    if (info.fragment != info.vertex) from += " + " + shader_label(*info.fragment);
    last_change_ = PipelineChange{"created graphics pipeline from " + from, frame_};
    return ManagedPipeline(std::move(slot));
}

std::expected<void, core::Error>
PipelineCache::check_compatible(const detail::PipelineSlot& slot, const std::string& key,
                                const ShaderReflection& updated) const {
    for (const auto& shader : slot.shaders) {
        const ShaderReflection* reflection = &updated;
        if (shader.path != key) {
            const LoadedShader* current = find(shader.path);
            if (current == nullptr) return fail(shader.path + " is not loaded");
            reflection = &current->reflection;
        }
        if (auto ok = check_reflection_compatible(shader.baseline, *reflection, shader.entries);
            !ok) {
            return fail("incompatible layout change in " + shader.path + ": " +
                        ok.error().message + " (restart to apply it)");
        }
    }
    return {};
}

// Re-creates `slot`'s pipeline from the cache's current shader modules (the
// caller has checked layout compatibility) and retires the old one.
std::expected<void, core::Error> PipelineCache::rebuild(detail::PipelineSlot& slot) {
    if (slot.is_compute) {
        ComputePipeline::CreateInfo info = slot.compute_info;
        info.shader = find(slot.shaders.front().path);
        auto pipeline = ComputePipeline::create(*device_ptr_, cache_, info);
        if (!pipeline) return std::unexpected(pipeline.error());
        retired_.push(frame_,
                      [old = std::make_shared<ComputePipeline>(std::move(slot.compute))]() mutable {
                          old.reset();
                      });
        slot.compute = std::move(*pipeline);
    } else {
        GraphicsPipeline::CreateInfo info = slot.graphics_info;
        info.vertex = find(slot.vertex_path);
        info.fragment = find(slot.fragment_path);
        auto pipeline = GraphicsPipeline::create(*device_ptr_, cache_, info);
        if (!pipeline) return std::unexpected(pipeline.error());
        retired_.push(frame_,
                      [old = std::make_shared<GraphicsPipeline>(std::move(slot.graphics))]() mutable {
                          old.reset();
                      });
        slot.graphics = std::move(*pipeline);
    }
    ++slot.generation;
    return {};
}

std::expected<PipelineReloadReport, core::Error>
PipelineCache::reload(const std::string& spv_path) {
    PipelineReloadReport report;
    const std::string key = shader_key(spv_path);
    const auto entry = modules_.find(key);
    if (entry == modules_.end()) {
        return report; // never loaded: the next load() reads the new file
    }
    report.shader_in_use = true;

    auto fresh = read_shader(key);
    if (!fresh) return std::unexpected(fresh.error());

    std::vector<std::shared_ptr<detail::PipelineSlot>> live;
    if (const auto list = slots_.find(key); list != slots_.end()) {
        std::erase_if(list->second, [](const auto& w) { return w.expired(); });
        for (const auto& weak : list->second) {
            if (auto slot = weak.lock()) live.push_back(std::move(slot));
        }
    }

    // All or nothing on layout compatibility: if any pipeline's layout cannot
    // take the new shader, keep the old module too, so pipelines created later
    // (with the same layouts) never pick up an incompatible shader.
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (auto ok = check_compatible(*live[i], key, fresh->reflection); !ok) {
            report.errors.push_back("pipeline " + std::to_string(i + 1) + ": " +
                                    ok.error().message);
        }
    }
    // The A10 check (storage images need a declared, device-supported
    // format) belongs to the same all-or-nothing decision: a shader that
    // fails it must not replace the running one.
    if (device_ptr_ != nullptr) {
        if (auto ok = device_ptr_->check_storage_image_formats(fresh->storage_image_formats, key);
            !ok) {
            report.errors.push_back(ok.error().message);
        }
    }
    if (!report.errors.empty()) {
        vkDestroyShaderModule(device_, fresh->module, nullptr); // never used
        return report;
    }

    // Update the cached entry in place (LoadedShader pointers stay valid); the
    // old module is retired along with the pipelines built from it.
    const VkDevice device = device_;
    const VkShaderModule old_module = entry->second.module;
    retired_.push(frame_, [device, old_module]() {
        vkDestroyShaderModule(device, old_module, nullptr);
    });
    entry->second.module = fresh->module;
    entry->second.reflection = std::move(fresh->reflection);
    entry->second.storage_image_formats = std::move(fresh->storage_image_formats);

    for (std::size_t i = 0; i < live.size(); ++i) {
        if (auto r = rebuild(*live[i]); !r) {
            report.errors.push_back("pipeline " + std::to_string(i + 1) + ": " +
                                    r.error().message);
        } else {
            ++report.rebuilt;
        }
    }
    if (report.rebuilt > 0) {
        last_change_ = PipelineChange{"rebuilt " + std::to_string(report.rebuilt) +
                                          " pipeline(s) after reloading " + key,
                                      frame_};
    }
    return report;
}

void PipelineCache::begin_frame(std::uint64_t frame_number) {
    frame_ = frame_number;
    retired_.collect(frame_number);
}

std::size_t PipelineCache::pipeline_count(const std::string& spv_path) const {
    const auto it = slots_.find(shader_key(spv_path));
    if (it == slots_.end()) return 0;
    return static_cast<std::size_t>(std::count_if(
        it->second.begin(), it->second.end(), [](const auto& w) { return !w.expired(); }));
}

void PipelineCache::destroy() noexcept {
    retired_.flush(); // the owner idles the device before destroying the cache
    slots_.clear();
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    for (auto& [key, shader] : modules_) {
        if (shader.module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, shader.module, nullptr);
        }
    }
    modules_.clear();
    if (cache_ != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device_, cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    device_ptr_ = nullptr;
}

PipelineCache::~PipelineCache() { destroy(); }

PipelineCache::PipelineCache(PipelineCache&& other) noexcept { *this = std::move(other); }

PipelineCache& PipelineCache::operator=(PipelineCache&& other) noexcept {
    if (this != &other) {
        destroy();
        device_ptr_ = std::exchange(other.device_ptr_, nullptr);
        device_     = std::exchange(other.device_, VK_NULL_HANDLE);
        cache_      = std::exchange(other.cache_, VK_NULL_HANDLE);
        modules_    = std::move(other.modules_);
        other.modules_.clear();
        slots_      = std::move(other.slots_);
        other.slots_.clear();
        retired_    = std::move(other.retired_);
        frame_      = std::exchange(other.frame_, 0);
        last_change_ = std::exchange(other.last_change_, std::nullopt);
    }
    return *this;
}

} // namespace engine::rhi
