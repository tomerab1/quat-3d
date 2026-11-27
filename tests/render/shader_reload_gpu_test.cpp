// Shader hot reload on the GPU (R2): a compute pipeline registered with the
// pipeline cache is rebuilt from an edited shader (watcher -> slangc ->
// PipelineCache::reload) and the next dispatch runs the new code; an edit that
// changes the descriptor layout is rejected and the old pipeline keeps
// running; replaced pipelines are destroyed only after the frames in flight.
// Needs slangc (skips without it).

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/descriptor_buffer.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/gpu_allocator.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/rhi/shader_compiler.hpp"
#include "engine/rhi/shader_hot_reload.hpp"
#include "support/expect_ok.hpp"
#include "support/gpu_context.hpp"
#include "support/gpu_submit.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
namespace rhi = engine::rhi;

void write_file(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// Writes `value` to g_out[0]; `extra_binding` adds a second buffer the
// pipeline layout does not have (an incompatible edit).
std::string compute_source(std::uint32_t value, bool extra_binding = false) {
    std::string s = "[[vk::binding(0)]] RWStructuredBuffer<uint> g_out;\n";
    if (extra_binding) s += "[[vk::binding(1)]] RWStructuredBuffer<uint> g_more;\n";
    s += "[shader(\"compute\")]\n[numthreads(1, 1, 1)]\n"
         "void compute_main(uint3 tid : SV_DispatchThreadID) {\n"
         "    g_out[0] = " + std::to_string(value) + "u;\n";
    if (extra_binding) s += "    g_more[0] = 1u;\n";
    s += "}\n";
    return s;
}

void compute_to_host_barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

TEST(GpuShaderReload, RebuiltComputePipelineRunsTheEditedShader) {
    QUAT_REQUIRE_GPU(g);
    if (!rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";

    quat::test::TempDir root("shader_reload_gpu");
    const fs::path src = root.path() / "shaders";
    const fs::path out = root.path() / "cooked";
    fs::create_directories(src);
    fs::create_directories(out);
    const fs::path source = src / "value.slang";
    write_file(source, compute_source(1));

    auto compiler = rhi::ShaderCompiler::create({{}, src, {}, out, {}});
    ASSERT_OK(compiler);
    const rhi::ShaderCompileResult first = compiler->compile(source);
    ASSERT_TRUE(first.ok) << first.diagnostics;

    // A private cache so the shared test cache's registry stays untouched.
    auto cache = rhi::PipelineCache::create(g->device);
    ASSERT_OK(cache);

    const rhi::DescriptorBufferFunctions fns =
        rhi::DescriptorBufferFunctions::load(g->device.handle());
    ASSERT_TRUE(fns.valid());
    const std::array<rhi::DescriptorBinding, 1> bindings = {
        rhi::DescriptorBinding{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                               VK_SHADER_STAGE_COMPUTE_BIT}};
    auto layout = rhi::DescriptorSetLayout::create(g->device, fns, bindings);
    ASSERT_OK(layout);

    const std::string spv = (out / "value.spv").string();
    auto shader = cache->load(spv);
    ASSERT_OK(shader);
    const VkDescriptorSetLayout set_layout = layout->handle();
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    auto pipeline = cache->create_compute_pipeline(info);
    ASSERT_OK(pipeline);
    EXPECT_EQ(cache->pipeline_count(spv), 1U);
    EXPECT_EQ(pipeline->generation(), 0U);

    auto buffer = g->allocator.create_buffer(
        16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VMA_MEMORY_USAGE_AUTO,
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    ASSERT_OK(buffer);
    auto descriptors = rhi::DescriptorBuffer::create(g->device, g->allocator, fns, *layout);
    ASSERT_OK(descriptors);
    descriptors->write_storage_buffer(0, rhi::buffer_device_address(g->device.handle(), *buffer),
                                      16);

    // Records with whatever the managed pipeline currently is, as passes do.
    const auto dispatch = [&]() -> std::uint32_t {
        std::memset(buffer->mapped(), 0, 16);
        auto r = quat::test::submit_and_wait(g->device, [&](VkCommandBuffer cmd) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->handle());
            descriptors->bind(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout(), 0);
            vkCmdDispatch(cmd, 1, 1, 1);
            compute_to_host_barrier(cmd);
        });
        EXPECT_OK(r);
        EXPECT_OK(buffer->invalidate_mapped());
        std::uint32_t value = 0;
        std::memcpy(&value, buffer->mapped(), sizeof(value));
        return value;
    };
    EXPECT_EQ(dispatch(), 1U);

    // ---- Edit the source; the hot-reload service rebuilds the pipeline. ------
    rhi::ShaderHotReload::Config config;
    config.source_dir = src;
    config.output_dir = out;
    config.debounce = std::chrono::milliseconds(0);
    config.poll_interval = std::chrono::milliseconds(0);
    config.async = false;
    auto reload = rhi::ShaderHotReload::create(*cache, config);
    ASSERT_OK(reload);
    std::uint64_t frame = 10;
    const auto run_frames_until = [&](auto done) {
        for (int i = 0; i < 2000 && !done(); ++i) {
            (*reload)->update(frame++);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };

    const VkPipeline original = pipeline->handle();
    write_file(source, compute_source(2));
    run_frames_until([&] { return pipeline->generation() == 1; });
    ASSERT_EQ(pipeline->generation(), 1U) << (*reload)->statuses().front().message;
    EXPECT_NE(pipeline->handle(), original);
    const rhi::ShaderStatus& status = (*reload)->statuses().front();
    EXPECT_EQ(status.state, rhi::ShaderStatus::State::ok) << status.message;
    EXPECT_EQ(status.pipelines, 1U);
    EXPECT_EQ(dispatch(), 2U) << "the rebuilt pipeline must run the edited shader";

    // ---- An edit adding a binding the layout lacks is rejected. -------------
    const VkPipeline rebuilt = pipeline->handle();
    write_file(source, compute_source(3, /*extra_binding=*/true));
    run_frames_until([&] {
        return (*reload)->statuses().front().state == rhi::ShaderStatus::State::error;
    });
    EXPECT_EQ((*reload)->statuses().front().state, rhi::ShaderStatus::State::error);
    EXPECT_NE((*reload)->statuses().front().message.find("binding 1"), std::string::npos)
        << (*reload)->statuses().front().message;
    EXPECT_EQ(pipeline->generation(), 1U);
    EXPECT_EQ(pipeline->handle(), rebuilt);
    EXPECT_EQ(dispatch(), 2U) << "the old pipeline keeps running after a rejected edit";

    // ---- Deferred destruction: retired objects die after frames in flight. --
    EXPECT_GT(cache->retired_count(), 0U);
    vkDeviceWaitIdle(g->device.handle());
    cache->begin_frame(frame + 2);
    EXPECT_EQ(cache->retired_count(), 0U);

    reload->reset();
}

TEST(GpuShaderReload, RetiresReplacedPipelinesAfterFramesInFlight) {
    QUAT_REQUIRE_GPU(g);
    if (!rhi::find_slangc()) GTEST_SKIP() << "slangc not found (QUAT_SLANGC / PATH)";

    quat::test::TempDir root("shader_reload_retire");
    const fs::path src = root.path() / "shaders";
    const fs::path out = root.path() / "cooked";
    fs::create_directories(src);
    write_file(src / "value.slang", compute_source(1));
    auto compiler = rhi::ShaderCompiler::create({{}, src, {}, out, {}});
    ASSERT_OK(compiler);
    ASSERT_TRUE(compiler->compile(src / "value.slang").ok);

    auto cache = rhi::PipelineCache::create(g->device);
    ASSERT_OK(cache);
    const rhi::DescriptorBufferFunctions fns =
        rhi::DescriptorBufferFunctions::load(g->device.handle());
    const std::array<rhi::DescriptorBinding, 1> bindings = {
        rhi::DescriptorBinding{0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                               VK_SHADER_STAGE_COMPUTE_BIT}};
    auto layout = rhi::DescriptorSetLayout::create(g->device, fns, bindings);
    ASSERT_OK(layout);
    const VkDescriptorSetLayout set_layout = layout->handle();

    const std::string spv = (out / "value.spv").string();
    auto shader = cache->load(spv);
    ASSERT_OK(shader);
    rhi::ComputePipeline::CreateInfo info{};
    info.shader = *shader;
    info.set_layouts = {&set_layout, 1};
    auto a = cache->create_compute_pipeline(info);
    auto b = cache->create_compute_pipeline(info);
    ASSERT_OK(a);
    ASSERT_OK(b);
    EXPECT_EQ(cache->pipeline_count(spv), 2U);

    write_file(src / "value.slang", compute_source(4));
    ASSERT_TRUE(compiler->compile(src / "value.slang").ok);
    cache->begin_frame(100); // the reload happens while recording frame 100
    auto report = cache->reload(spv);
    ASSERT_OK(report);
    EXPECT_TRUE(report->shader_in_use);
    EXPECT_EQ(report->rebuilt, 2U);
    EXPECT_TRUE(report->errors.empty());
    // Two pipelines + the old shader module wait for frames 100 and 101.
    EXPECT_EQ(cache->retired_count(), 3U);
    cache->begin_frame(101);
    EXPECT_EQ(cache->retired_count(), 3U) << "frame 100 may still be executing";
    vkDeviceWaitIdle(g->device.handle());
    cache->begin_frame(102);
    EXPECT_EQ(cache->retired_count(), 0U);

    // Dropping a pipeline unregisters it.
    *b = rhi::ManagedPipeline{};
    EXPECT_EQ(cache->pipeline_count(spv), 1U);
}

} // namespace
