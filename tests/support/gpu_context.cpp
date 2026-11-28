#include "gpu_context.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"

namespace quat::test {

namespace {

// Owns everything GpuContext refers to. Destroyed in reverse order of
// creation (pool and caches before the device).
struct GpuState {
    engine::rhi::Device                       device;
    std::optional<engine::rhi::GpuAllocator>  allocator;
    std::optional<engine::rhi::PipelineCache> cache;
    VkCommandPool                             pool = VK_NULL_HANDLE;
    std::optional<engine::rhi::TransferContext> transfer;
    std::optional<GpuContext>                 context;

    ~GpuState() {
        if (device.handle() == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(device.handle());
        context.reset();
        transfer.reset();
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device.handle(), pool, nullptr);
        cache.reset();
        allocator.reset();
    }
};

struct Registry {
    bool                      attempted = false;
    std::string               failure;
    std::unique_ptr<GpuState> state;
};

Registry& registry() {
    static Registry r;
    return r;
}

std::atomic<std::size_t> g_errors_outside_tests{0};

[[nodiscard]] bool validation_layer_installed() {
    std::uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS) return false;
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return true;
    }
    return false;
}

} // namespace

GpuOptions& gpu_options() {
    static GpuOptions options;
    return options;
}

GpuContext* gpu_context(std::string& why) {
    Registry& reg = registry();
    if (!reg.attempted) {
        reg.attempted = true;
        // Device::create quietly drops validation when the layer is missing;
        // a run that asked for it must not pass without it.
        if (gpu_options().validation && !validation_layer_installed()) {
            reg.failure = "validation was requested (--validation / QUAT_VALIDATION) but "
                          "VK_LAYER_KHRONOS_validation is not installed";
            why = reg.failure;
            return nullptr;
        }
        auto device = engine::rhi::Device::create({.enable_validation = gpu_options().validation,
                                                   .application_name = "engine_tests",
                                                   .on_debug_message = report_validation_message});
        if (!device) {
            reg.failure = device.error().message;
        } else {
            auto state = std::make_unique<GpuState>();
            state->device = std::move(*device);
            auto allocator = engine::rhi::GpuAllocator::create(state->device);
            auto cache = engine::rhi::PipelineCache::create(state->device);
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            // Graphics family: mip-generation blits are only legal there.
            pool_info.queueFamilyIndex = state->device.queue_families().graphics;
            if (!allocator) {
                reg.failure = allocator.error().message;
            } else if (!cache) {
                reg.failure = cache.error().message;
            } else if (vkCreateCommandPool(state->device.handle(), &pool_info, nullptr,
                                           &state->pool) != VK_SUCCESS) {
                reg.failure = "command pool creation failed";
            } else {
                state->allocator.emplace(std::move(*allocator));
                state->cache.emplace(std::move(*cache));
                state->transfer.emplace(engine::rhi::TransferContext{
                    state->device.handle(), state->pool, state->device.graphics_queue(),
                    state->device.max_sampler_anisotropy(),
                    state->device.texture_caps()});
                state->context.emplace(GpuContext{state->device, *state->allocator,
                                                  *state->cache, *state->transfer,
                                                  QUAT_COOKED_SHADER_DIR});
                reg.state = std::move(state);
            }
        }
    }
    if (!reg.state) {
        why = reg.failure;
        return nullptr;
    }
    return &*reg.state->context;
}

void shutdown_gpu_context() { registry().state.reset(); }

void report_validation_message(engine::rhi::DebugMessageSeverity severity,
                               std::string_view message) {
    if (severity != engine::rhi::DebugMessageSeverity::error) return;
    const testing::TestInfo* test = testing::UnitTest::GetInstance()->current_test_info();
    if (test == nullptr) {
        g_errors_outside_tests.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "Vulkan validation error outside any test: %.*s\n",
                     static_cast<int>(message.size()), message.data());
        return;
    }
    ADD_FAILURE() << "Vulkan validation error in " << test->test_suite_name() << "."
                  << test->name() << ": " << message;
}

std::size_t validation_errors_outside_tests() {
    return g_errors_outside_tests.load(std::memory_order_relaxed);
}

} // namespace quat::test
