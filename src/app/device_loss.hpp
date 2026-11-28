#pragma once

// The GPU fault path (issue #110): what the application does when the frame
// loop stops on a GpuFault (frame_sync.hpp) — device lost, a GPU hang, or a
// failed submit / present.
//
//   1. log a clear "[fatal] GPU device lost" (or hang / submit failure) line,
//      plus the last pipeline change (a freshly created or hot-reloaded
//      shader — e.g. a model-authored material — is the usual suspect) and,
//      where the driver offers VK_EXT_device_fault, its fault report;
//   2. write a recovery save under <project root>/recovery/ with the
//      existing scene_io: the edit scene, and while playing also the play
//      state (the edit scene is then the play-mode snapshot);
//   3. the caller exits with k_exit_gpu_fault.
//
// Rendering never resumes: a lost device cannot be recovered in place.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/frame_sync.hpp"
#include "engine/core/error.hpp"

namespace engine::renderer {
struct RenderSettings;
}
namespace engine::rhi {
class Device;
struct PipelineChange;
} // namespace engine::rhi
namespace engine::scene {
class Scene;
struct SceneEnvironment;
struct SceneSnapshot;
} // namespace engine::scene

namespace quat::app {

// The process exit status after a GPU fault.
inline constexpr int k_exit_gpu_fault = 3;

// What a recovery save holds.
struct RecoveryInputs {
    // The live scene: the edit scene, or the play state while playing.
    const engine::scene::Scene* scene = nullptr;
    // While playing: the edit scene as Play captured it (PlayMode). Null
    // outside play.
    const engine::scene::SceneSnapshot* edit_snapshot = nullptr;
    const engine::renderer::RenderSettings* render_settings = nullptr; // optional
    const engine::scene::SceneEnvironment*  environment = nullptr;     // optional
};

struct RecoveryFiles {
    std::filesystem::path                edit; // the edit scene
    std::optional<std::filesystem::path> play; // the play state, when playing
};

// The recovery directory under a project root.
[[nodiscard]] std::filesystem::path recovery_dir(const std::filesystem::path& project_root);

// Writes <project_root>/recovery/<stem>.scene.json (the edit scene) and, while
// playing, <stem>.play.scene.json (the play state). Fails when a file cannot
// be written; the edit scene is written first.
[[nodiscard]] std::expected<RecoveryFiles, engine::core::Error>
write_recovery_save(const RecoveryInputs& in, const std::filesystem::path& project_root,
                    std::string_view stem);

// "device-lost-<unix seconds>" (or "gpu-hang-...", "gpu-fault-..."): a
// recovery file stem that does not overwrite an earlier recovery.
[[nodiscard]] std::string recovery_stem(const GpuFault& fault);

// The log lines for a fault: the fault itself, then the last pipeline change
// when one is known (`frame` is the frame the fault ended).
[[nodiscard]] std::vector<std::string>
describe_fault_context(const GpuFault& fault, const engine::rhi::PipelineChange* last_change,
                       std::uint64_t frame);

// VK_EXT_device_fault's report for a lost device, one line per entry; empty
// when the extension is not enabled or reports nothing.
[[nodiscard]] std::vector<std::string> query_device_fault(const engine::rhi::Device& device);

// Steps 1 and 2 above: logs the fault (with `device`'s VK_EXT_device_fault
// report when lost) and writes the recovery save. Returns k_exit_gpu_fault.
[[nodiscard]] int handle_gpu_fault(const GpuFault& fault, const engine::rhi::Device& device,
                                   const engine::rhi::PipelineChange* last_change,
                                   std::uint64_t frame, const RecoveryInputs& recovery,
                                   const std::filesystem::path& project_root);

} // namespace quat::app
