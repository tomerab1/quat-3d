#include "engine/rhi/pipeline_cache.hpp"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

#include "engine/rhi/device.hpp"

namespace engine::rhi {

namespace {

[[nodiscard]] std::unexpected<core::Error> fail(std::string what) {
    return std::unexpected(core::Error{std::move(what)});
}

[[nodiscard]] std::expected<std::vector<std::uint32_t>, core::Error>
read_spirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
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
    std::ifstream file(path, std::ios::binary);
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

} // namespace

std::expected<PipelineCache, core::Error> PipelineCache::create(const Device& device) {
    PipelineCache out;
    out.device_ = device.handle();

    VkPipelineCacheCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (VkResult r = vkCreatePipelineCache(out.device_, &info, nullptr, &out.cache_);
        r != VK_SUCCESS) {
        return fail("vkCreatePipelineCache failed");
    }
    return out;
}

std::expected<const LoadedShader*, core::Error>
PipelineCache::load(const std::string& spv_path) {
    const std::size_t key = std::hash<std::string>{}(spv_path);
    if (auto it = modules_.find(key); it != modules_.end()) {
        return &it->second;
    }

    auto words = read_spirv(spv_path);
    if (!words) return std::unexpected(words.error());

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = words->size() * sizeof(std::uint32_t);
    module_info.pCode = words->data();

    VkShaderModule module = VK_NULL_HANDLE;
    if (VkResult r = vkCreateShaderModule(device_, &module_info, nullptr, &module);
        r != VK_SUCCESS) {
        return fail("vkCreateShaderModule failed for " + spv_path);
    }

    auto reflection_text = read_text(reflection_path_for(spv_path));
    if (!reflection_text) {
        vkDestroyShaderModule(device_, module, nullptr);
        return std::unexpected(reflection_text.error());
    }
    auto reflection = parse_reflection_json(*reflection_text);
    if (!reflection) {
        vkDestroyShaderModule(device_, module, nullptr);
        return std::unexpected(reflection.error());
    }

    auto [it, inserted] = modules_.emplace(
        key, LoadedShader{module, std::move(*reflection)});
    return &it->second;
}

void PipelineCache::destroy() noexcept {
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
}

PipelineCache::~PipelineCache() { destroy(); }

PipelineCache::PipelineCache(PipelineCache&& other) noexcept { *this = std::move(other); }

PipelineCache& PipelineCache::operator=(PipelineCache&& other) noexcept {
    if (this != &other) {
        destroy();
        device_  = std::exchange(other.device_, VK_NULL_HANDLE);
        cache_   = std::exchange(other.cache_, VK_NULL_HANDLE);
        modules_ = std::move(other.modules_);
    }
    return *this;
}

} // namespace engine::rhi
