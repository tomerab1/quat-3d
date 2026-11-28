// Issue #110: the device-lost path writes a recovery save of the edit scene
// (and, while playing, of the play state) under the project root, and logs
// the last pipeline change. Driven with a fake device-lost result: a
// default-constructed Device stands for the lost one (no VK_EXT_device_fault).

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "app/device_loss.hpp"
#include "app/frame_sync.hpp"
#include "app/play_mode.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_snapshot.hpp"
#include "support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using quat::app::GpuFault;
using quat::app::GpuFaultKind;

[[nodiscard]] std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool names(const std::string& json, const std::string& entity) {
    return json.find("\"" + entity + "\"") != std::string::npos;
}

// The recovery files written under `project`.
[[nodiscard]] std::vector<fs::path> recovery_files(const fs::path& project) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(quat::app::recovery_dir(project), ec)) {
        out.push_back(e.path());
    }
    return out;
}

TEST(DeviceLoss, RecoverySaveWritesTheEditScene) {
    quat::test::TempDir project("device-loss-edit");
    engine::scene::Scene scene;
    (void)scene.create_entity("authored crate");
    const quat::app::RecoveryInputs in{.scene = &scene};
    const auto files = quat::app::write_recovery_save(in, project.path(), "device-lost-1");
    ASSERT_TRUE(files.has_value()) << files.error().message;
    EXPECT_EQ(files->edit, project.path() / "recovery" / "device-lost-1.scene.json");
    EXPECT_FALSE(files->play.has_value());
    EXPECT_TRUE(names(read_file(files->edit), "authored crate"));
}

TEST(DeviceLoss, WhilePlayingTheEditSceneComesFromThePlaySnapshot) {
    quat::test::TempDir project("device-loss-play");
    engine::scene::Scene scene;
    (void)scene.create_entity("authored crate");
    quat::app::PlayMode play;
    *play.request() = true;
    ASSERT_TRUE(play.apply(scene, {}));
    ASSERT_NE(play.edit_snapshot(), nullptr);
    (void)scene.create_entity("spawned during play");

    const quat::app::RecoveryInputs in{.scene = &scene, .edit_snapshot = play.edit_snapshot()};
    const auto files = quat::app::write_recovery_save(in, project.path(), "device-lost-2");
    ASSERT_TRUE(files.has_value()) << files.error().message;
    ASSERT_TRUE(files->play.has_value());
    const std::string edit = read_file(files->edit);
    const std::string played = read_file(*files->play);
    EXPECT_TRUE(names(edit, "authored crate"));
    EXPECT_FALSE(names(edit, "spawned during play")) << "the edit scene is the snapshot";
    EXPECT_TRUE(names(played, "authored crate"));
    EXPECT_TRUE(names(played, "spawned during play"));

    *play.request() = false;
    ASSERT_TRUE(play.apply(scene, {}));
    EXPECT_EQ(play.edit_snapshot(), nullptr) << "no snapshot outside play";
}

TEST(DeviceLoss, FaultContextNamesTheLastPipelineChange) {
    const engine::rhi::PipelineChange change{
        "created graphics pipeline from cooked/shaders/materials/lava.gbuffer.spv", 40};
    const auto lines = quat::app::describe_fault_context(
        GpuFault{GpuFaultKind::device_lost, VK_ERROR_DEVICE_LOST, "vkWaitForFences"}, &change,
        42);
    ASSERT_GE(lines.size(), 2U);
    EXPECT_NE(lines[0].find("GPU device lost"), std::string::npos) << lines[0];
    EXPECT_NE(lines[0].find("frame 42"), std::string::npos) << lines[0];
    EXPECT_NE(lines[1].find("lava.gbuffer.spv"), std::string::npos) << lines[1];
    EXPECT_NE(lines[1].find("2 frame(s) before"), std::string::npos) << lines[1];

    const auto none = quat::app::describe_fault_context(
        GpuFault{GpuFaultKind::submit_failed, VK_ERROR_OUT_OF_HOST_MEMORY, "vkQueueSubmit2"},
        nullptr, 3);
    ASSERT_EQ(none.size(), 1U) << "no pipeline line without a change";
}

// The whole path from the frame loop's point of view: a submit reports
// VK_ERROR_DEVICE_LOST, the loop's fault goes to handle_gpu_fault, which
// writes the recovery file and returns the exit status.
class LostQueue final : public quat::app::QueueSyncOps {
public:
    VkResult create_fence(bool, VkFence* out) override {
        *out = VK_NULL_HANDLE;
        return VK_SUCCESS;
    }
    void     destroy_fence(VkFence) override {}
    VkResult wait_fence(VkFence, std::uint64_t) override { return VK_SUCCESS; }
    VkResult reset_fence(VkFence) override { return VK_SUCCESS; }
    VkResult submit(std::span<const VkSubmitInfo2>, VkFence) override {
        return VK_ERROR_DEVICE_LOST;
    }
};

TEST(DeviceLoss, DeviceLostFromTheFrameLoopWritesTheRecoveryFile) {
    quat::test::TempDir project("device-loss-loop");
    engine::scene::Scene scene;
    (void)scene.create_entity("unsaved work");

    quat::app::FrameFences fences;
    ASSERT_TRUE(fences.create(std::make_unique<LostQueue>(), 2, 1'000'000'000ULL).has_value());
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    ASSERT_FALSE(fences.submit(0, submit).has_value());
    ASSERT_TRUE(fences.fault().has_value());
    ASSERT_EQ(fences.fault()->kind, GpuFaultKind::device_lost);

    const engine::rhi::Device lost_device; // no VK_EXT_device_fault report
    const engine::rhi::PipelineChange change{"rebuilt 1 pipeline(s) after reloading x.spv", 7};
    const int status = quat::app::handle_gpu_fault(*fences.fault(), lost_device, &change, 9,
                                                   {.scene = &scene}, project.path());
    EXPECT_EQ(status, quat::app::k_exit_gpu_fault);
    EXPECT_NE(status, 0);

    const std::vector<fs::path> files = recovery_files(project.path());
    ASSERT_EQ(files.size(), 1U);
    const std::string name = files.front().filename().string();
    EXPECT_TRUE(name.starts_with("device-lost-")) << name;
    EXPECT_TRUE(name.ends_with(".scene.json")) << name;
    EXPECT_TRUE(names(read_file(files.front()), "unsaved work"));
}

TEST(DeviceLoss, RecoveryStemsNameTheFault) {
    EXPECT_TRUE(quat::app::recovery_stem({GpuFaultKind::device_lost, VK_ERROR_DEVICE_LOST, ""})
                    .starts_with("device-lost-"));
    EXPECT_TRUE(quat::app::recovery_stem({GpuFaultKind::hang, VK_TIMEOUT, ""})
                    .starts_with("gpu-hang-"));
    EXPECT_TRUE(quat::app::recovery_stem({GpuFaultKind::submit_failed, VK_ERROR_UNKNOWN, ""})
                    .starts_with("gpu-fault-"));
    EXPECT_TRUE(quat::app::query_device_fault(engine::rhi::Device{}).empty());
}

} // namespace
