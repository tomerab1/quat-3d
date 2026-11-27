// engine_tests — the headless test runner (ROADMAP S3).
//
// Runs every library self-test without a window: the device-free (CPU) tests
// always, and the GPU tests on a surface-less Vulkan device — a real GPU, or
// a software implementation such as Mesa's lavapipe on CI machines
// (VK_ICD_FILENAMES=.../lvp_icd.json). Exit status = number of failures, so
// CI and scripts can gate on it.
//
//   engine_tests                 run everything available
//   engine_tests --list          list test names
//   engine_tests --cpu           CPU tests only
//   engine_tests --require-gpu   fail (instead of skip) when no Vulkan device
//   engine_tests --validation    enable the Khronos validation layer
//   engine_tests <substring>...  run only tests whose name contains one
//
// Self-tests that still live in src/main.cpp (they depend on frame-loop
// helpers) run under QUAT_SELFTEST until the main.cpp decomposition (S7)
// moves them here.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <vulkan/vulkan.h>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/asset/material_asset.hpp"
#include "engine/core/error.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/prediction.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/transport.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/renderer/atmosphere_pass.hpp"
#include "engine/renderer/bloom_pass.hpp"
#include "engine/renderer/dynamic_ibl.hpp"
#include "engine/renderer/exposure_pass.hpp"
#include "engine/renderer/ibl_pass.hpp"
#include "engine/renderer/lighting_pass.hpp"
#include "engine/renderer/mesh_pass.hpp"
#include "engine/renderer/skinning_pass.hpp"
#include "engine/renderer/taa_pass.hpp"
#include "engine/renderer/terrain_pass.hpp"
#include "engine/renderer/tonemap_pass.hpp"
#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/render_graph.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/streamer.hpp"

#ifdef ENGINE_EDITOR
#include "../src/editor/scene_snapshot.hpp"
#include "engine/renderer/imgui_pass.hpp"
#endif

namespace {

using engine::core::Error;
using Result = std::expected<void, Error>;

// Everything a GPU test may need, created once for the whole run.
struct GpuContext {
    engine::rhi::Device&                device;
    engine::rhi::GpuAllocator&          allocator;
    engine::rhi::PipelineCache&         cache;
    const engine::rhi::TransferContext& transfer;
    std::string                         shader_dir;
};

struct TestCase {
    std::string                             name;
    std::function<Result()>                 cpu; // set for device-free tests
    std::function<Result(GpuContext&)>      gpu; // set for device tests
};

std::vector<TestCase> make_tests() {
    namespace a = engine::animation;
    namespace r = engine::renderer;
    namespace s = engine::scene;
    std::vector<TestCase> t;
    const auto cpu = [&t](std::string name, std::function<Result()> fn) {
        t.push_back({std::move(name), std::move(fn), {}});
    };
    const auto gpu = [&t](std::string name, std::function<Result(GpuContext&)> fn) {
        t.push_back({std::move(name), {}, std::move(fn)});
    };

    // ---- Device-free -------------------------------------------------------
    cpu("asset/manager", [] { return engine::asset::run_asset_manager_self_test(); });
    cpu("scene/ecs", [] { return s::run_ecs_self_test(); });
    cpu("scene/systems", [] { return s::run_scene_self_test(); });
#ifdef ENGINE_EDITOR
    cpu("editor/snapshot", [] { return engine::editor::run_scene_snapshot_self_test(); });
#endif
    cpu("anim/skeleton", [] { return a::run_skeleton_self_test(); });
    cpu("anim/clip", [] { return a::run_clip_self_test(); });
    cpu("anim/animator", [] { return a::run_animator_self_test(); });
    cpu("anim/blend", [] { return a::run_blend_self_test(); });
    cpu("anim/state_machine", [] { return a::run_state_machine_self_test(); });
    cpu("anim/skeleton_load", [] { return s::run_skeleton_load_self_test(); });
    cpu("anim/clip_load", [] { return s::run_animation_load_self_test(); });
    cpu("physics/world", [] { return engine::physics::run_physics_self_test(); });
    cpu("physics/bodies", [] { return s::run_physics_body_self_test(); });
    cpu("physics/terrain", [] { return s::run_terrain_physics_self_test(); });
    cpu("physics/character", [] { return s::run_character_self_test(); });
    cpu("physics/triggers", [] { return engine::physics::run_trigger_self_test(); });
    cpu("gltf/material_extract", [] { return s::run_material_extract_self_test(); });
    cpu("gltf/validation", [] { return s::run_gltf_validation_self_test(); });
    cpu("terrain/generate", [] { return engine::terrain::run_terrain_self_test(); });
    cpu("terrain/streaming", [] { return engine::terrain::run_terrain_streaming_self_test(); });
    cpu("nav/navmesh", [] { return engine::nav::run_navmesh_self_test(); });
    cpu("nav/agents", [] { return s::run_nav_agent_self_test(); });
    cpu("nav/patrol", [] { return s::run_patrol_self_test(); });
    cpu("ai/behavior", [] { return s::run_behavior_self_test(); });
    cpu("net/transport", [] { return engine::net::run_transport_self_test(); });
    cpu("net/replication", [] { return engine::net::run_replication_self_test(); });
    cpu("net/prediction", [] { return engine::net::run_prediction_self_test(); });
    cpu("remote/rpc_server", [] { return engine::remote::run_rpc_server_self_test(); });

    // ---- GPU ---------------------------------------------------------------
    gpu("rhi/gpu_round_trip",
        [](GpuContext& g) { return engine::rhi::run_gpu_round_trip_self_test(g.device, g.allocator); });
    gpu("rhi/descriptor_buffer", [](GpuContext& g) {
        return engine::rhi::run_descriptor_buffer_self_test(g.device, g.allocator);
    });
    gpu("rhi/render_graph", [](GpuContext& g) {
        engine::rhi::TransientImagePool pool(g.device, g.allocator);
        return engine::rhi::run_render_graph_self_test(g.device, g.allocator, pool);
    });
    gpu("gltf/loader",
        [](GpuContext& g) { return s::run_gltf_loader_self_test(g.allocator, g.transfer); });
    gpu("gltf/textures",
        [](GpuContext& g) { return s::run_texture_loader_self_test(g.allocator, g.transfer); });
    gpu("gltf/scene",
        [](GpuContext& g) { return s::run_gltf_scene_self_test(g.allocator, g.transfer); });
    gpu("asset/material", [](GpuContext& g) {
        return engine::asset::run_material_asset_self_test(g.allocator, g.transfer);
    });
    gpu("scene/io", [](GpuContext& g) { return s::run_scene_io_self_test(g.allocator, g.transfer); });

    // Passes taking (device, allocator, cache, transfer, shader_dir).
    using FullPassTest = Result (*)(const engine::rhi::Device&, engine::rhi::GpuAllocator&,
                                    engine::rhi::PipelineCache&, const engine::rhi::TransferContext&,
                                    const std::string&);
    const auto full = [&gpu](std::string name, FullPassTest fn) {
        gpu(std::move(name), [fn](GpuContext& g) {
            return fn(g.device, g.allocator, g.cache, g.transfer, g.shader_dir);
        });
    };
    full("render/gbuffer", &r::run_mesh_pass_self_test);
    full("render/lighting", &r::run_lighting_pass_self_test);
    full("render/point_lights", &r::run_point_light_self_test);
    full("render/clearcoat", &r::run_clearcoat_self_test);
    full("render/specular_aa", &r::run_specular_aa_self_test);
    full("render/terrain", &r::run_terrain_pass_self_test);
    full("render/ibl_lighting", &r::run_ibl_lighting_self_test);
    full("render/tonemap", &r::run_tonemap_pass_self_test);
    full("render/skinning", &r::run_skinning_pass_self_test);
#ifdef ENGINE_EDITOR
    full("render/imgui", &r::run_imgui_pass_self_test);
#endif

    // Passes taking (device, allocator, cache, shader_dir).
    using ShaderPassTest = Result (*)(const engine::rhi::Device&, engine::rhi::GpuAllocator&,
                                      engine::rhi::PipelineCache&, const std::string&);
    const auto shaders = [&gpu](std::string name, ShaderPassTest fn) {
        gpu(std::move(name),
            [fn](GpuContext& g) { return fn(g.device, g.allocator, g.cache, g.shader_dir); });
    };
    shaders("render/atmosphere", &r::run_atmosphere_self_test);
    shaders("render/ibl", &r::run_ibl_self_test);
    shaders("render/dynamic_ibl", &r::run_dynamic_ibl_self_test);
    shaders("render/bloom", &r::run_bloom_self_test);
    shaders("render/exposure", &r::run_exposure_self_test);
    shaders("render/taa", &r::run_taa_self_test);
    return t;
}

struct Options {
    bool list = false;
    bool cpu_only = false;
    bool require_gpu = false;
    bool validation = false;
    std::vector<std::string> filters;
};

[[nodiscard]] bool selected(const Options& o, const std::string& name) {
    if (o.filters.empty()) return true;
    for (const std::string& f : o.filters) {
        if (name.find(f) != std::string::npos) return true;
    }
    return false;
}

[[nodiscard]] int run_one(const std::string& name, const std::function<Result()>& fn) {
    const auto start = std::chrono::steady_clock::now();
    const Result r = fn();
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    if (r) {
        std::printf("  ok    %-28s %8.1f ms\n", name.c_str(), ms);
        return 0;
    }
    std::printf("  FAIL  %-28s %8.1f ms  %s\n", name.c_str(), ms, r.error().message.c_str());
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--list") {
            opt.list = true;
        } else if (arg == "--cpu") {
            opt.cpu_only = true;
        } else if (arg == "--require-gpu") {
            opt.require_gpu = true;
        } else if (arg == "--validation") {
            opt.validation = true;
        } else if (arg.starts_with("--")) {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            return 2;
        } else {
            opt.filters.emplace_back(arg);
        }
    }

    const std::vector<TestCase> tests = make_tests();
    if (opt.list) {
        for (const TestCase& t : tests) std::printf("%s %s\n", t.cpu ? "cpu" : "gpu", t.name.c_str());
        return 0;
    }

    int failures = 0;
    int ran = 0;
    std::printf("[engine_tests] CPU tests\n");
    for (const TestCase& t : tests) {
        if (!t.cpu || !selected(opt, t.name)) continue;
        failures += run_one(t.name, t.cpu);
        ++ran;
    }

    bool wants_gpu = false;
    for (const TestCase& t : tests) wants_gpu = wants_gpu || (t.gpu && selected(opt, t.name));
    if (!opt.cpu_only && wants_gpu) {
        auto device = engine::rhi::Device::create({.enable_validation = opt.validation,
                                                   .application_name = "engine_tests"});
        if (!device) {
            std::printf("[engine_tests] GPU tests %s: no Vulkan device (%s)\n",
                        opt.require_gpu ? "FAILED" : "skipped", device.error().message.c_str());
            if (opt.require_gpu) ++failures;
        } else {
            auto allocator = engine::rhi::GpuAllocator::create(*device);
            auto cache = engine::rhi::PipelineCache::create(*device);
            VkCommandPool pool = VK_NULL_HANDLE;
            VkCommandPoolCreateInfo pool_info{};
            pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            // Graphics family: mip-generation blits are only legal there.
            pool_info.queueFamilyIndex = device->queue_families().graphics;
            if (!allocator || !cache ||
                vkCreateCommandPool(device->handle(), &pool_info, nullptr, &pool) != VK_SUCCESS) {
                std::printf("[engine_tests] GPU setup FAILED: %s\n",
                            !allocator ? allocator.error().message.c_str()
                            : !cache   ? cache.error().message.c_str()
                                       : "command pool");
                ++failures;
            } else {
                const engine::rhi::TransferContext transfer{device->handle(), pool,
                                                            device->graphics_queue(),
                                                            device->max_sampler_anisotropy()};
                GpuContext ctx{*device, *allocator, *cache, transfer, QUAT_COOKED_SHADER_DIR};
                std::printf("[engine_tests] GPU tests\n");
                for (const TestCase& t : tests) {
                    if (!t.gpu || !selected(opt, t.name)) continue;
                    failures += run_one(t.name, [&] { return t.gpu(ctx); });
                    ++ran;
                }
                vkDeviceWaitIdle(device->handle());
                vkDestroyCommandPool(device->handle(), pool, nullptr);
            }
        }
    }

    std::printf("[engine_tests] %d run, %d failed\n", ran, failures);
    return failures;
}
