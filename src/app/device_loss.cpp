#include "app/device_loss.hpp"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <utility>

#include "engine/core/utf8_path.hpp"
#include "engine/rhi/device.hpp"
#include "engine/rhi/pipeline_cache.hpp"
#include "engine/scene/scene.hpp"
#include "engine/scene/scene_io.hpp"
#include "engine/scene/scene_snapshot.hpp"

namespace quat::app {
namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string hex(std::uint64_t value) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016" PRIx64, value);
    return buf;
}

[[nodiscard]] const char* address_type_name(VkDeviceFaultAddressTypeEXT type) {
    switch (type) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "invalid read";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "invalid write";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "invalid execute";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "instruction pointer (fault)";
    default: return "unknown";
    }
}

[[nodiscard]] std::expected<void, engine::core::Error>
save_to(const engine::scene::Scene& scene, const fs::path& path, const RecoveryInputs& in) {
    return engine::scene::save_scene(scene, path, in.render_settings, in.environment);
}

} // namespace

fs::path recovery_dir(const fs::path& project_root) {
    return project_root / "recovery";
}

std::expected<RecoveryFiles, engine::core::Error>
write_recovery_save(const RecoveryInputs& in, const fs::path& project_root,
                    std::string_view stem) {
    if (in.scene == nullptr) {
        return std::unexpected(engine::core::Error{"recovery save: no scene"});
    }
    const fs::path dir = recovery_dir(project_root);
    const std::string base(stem);
    RecoveryFiles files;
    files.edit = dir / (base + ".scene.json");
    if (in.edit_snapshot != nullptr) {
        // Playing: the edit scene is the snapshot Play took. Rebuild it in a
        // scratch scene (exact entity ids) and save that; the live scene is
        // the play state, saved next to it.
        engine::scene::Scene edit;
        engine::scene::restore_scene(edit, *in.edit_snapshot);
        if (auto r = save_to(edit, files.edit, in); !r) return std::unexpected(r.error());
        files.play = dir / (base + ".play.scene.json");
        if (auto r = save_to(*in.scene, *files.play, in); !r) return std::unexpected(r.error());
    } else if (auto r = save_to(*in.scene, files.edit, in); !r) {
        return std::unexpected(r.error());
    }
    return files;
}

std::string recovery_stem(const GpuFault& fault) {
    const char* prefix = "gpu-fault";
    if (fault.kind == GpuFaultKind::device_lost) prefix = "device-lost";
    if (fault.kind == GpuFaultKind::hang) prefix = "gpu-hang";
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return std::string(prefix) + "-" + std::to_string(seconds.count());
}

std::vector<std::string> describe_fault_context(const GpuFault& fault,
                                                const engine::rhi::PipelineChange* last_change,
                                                std::uint64_t frame) {
    std::vector<std::string> lines;
    lines.push_back(describe_gpu_fault(fault) + " (frame " + std::to_string(frame) + ")");
    if (last_change != nullptr) {
        const std::uint64_t ago = frame >= last_change->frame ? frame - last_change->frame : 0;
        lines.push_back("last pipeline change: " + last_change->what + " (frame " +
                        std::to_string(last_change->frame) + ", " + std::to_string(ago) +
                        " frame(s) before the fault)");
    }
    if (fault.kind == GpuFaultKind::device_lost || fault.kind == GpuFaultKind::hang) {
        lines.emplace_back("a shader that never terminates (an unbounded loop) or reads out of "
                           "bounds is the usual cause; check the material or shader changed "
                           "last");
    }
    return lines;
}

std::vector<std::string> query_device_fault(const engine::rhi::Device& device) {
    std::vector<std::string> lines;
    if (!device.device_fault() || device.handle() == VK_NULL_HANDLE) return lines;
    const auto get_info = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(
        vkGetDeviceProcAddr(device.handle(), "vkGetDeviceFaultInfoEXT"));
    if (get_info == nullptr) return lines;

    VkDeviceFaultCountsEXT counts{};
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    if (get_info(device.handle(), &counts, nullptr) != VK_SUCCESS) return lines;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT>  vendor(counts.vendorInfoCount);
    counts.vendorBinarySize = 0; // not requested (deviceFaultVendorBinary off)
    VkDeviceFaultInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
    info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
    const VkResult r = get_info(device.handle(), &counts, &info);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return lines;

    lines.push_back(std::string("device fault: ") + info.description);
    for (std::uint32_t i = 0; i < counts.addressInfoCount && i < addresses.size(); ++i) {
        const VkDeviceFaultAddressInfoEXT& a = addresses[i];
        lines.push_back(std::string("  address: ") + address_type_name(a.addressType) + " at " +
                        hex(a.reportedAddress) + " (precision " + hex(a.addressPrecision) + ")");
    }
    for (std::uint32_t i = 0; i < counts.vendorInfoCount && i < vendor.size(); ++i) {
        const VkDeviceFaultVendorInfoEXT& v = vendor[i];
        lines.push_back(std::string("  vendor: ") + v.description + " (code " +
                        hex(v.vendorFaultCode) + ", data " + hex(v.vendorFaultData) + ")");
    }
    return lines;
}

int handle_gpu_fault(const GpuFault& fault, const engine::rhi::Device& device,
                     const engine::rhi::PipelineChange* last_change, std::uint64_t frame,
                     const RecoveryInputs& recovery, const fs::path& project_root) {
    for (const std::string& line : describe_fault_context(fault, last_change, frame)) {
        std::fprintf(stderr, "[fatal] %s\n", line.c_str());
    }
    if (fault.kind == GpuFaultKind::device_lost) {
        for (const std::string& line : query_device_fault(device)) {
            std::fprintf(stderr, "[fatal] %s\n", line.c_str());
        }
    }
    if (auto saved = write_recovery_save(recovery, project_root, recovery_stem(fault)); saved) {
        std::fprintf(stderr, "[fatal] recovery save of the edit scene: %s\n",
                     engine::core::path_to_utf8(saved->edit).c_str());
        if (saved->play) {
            std::fprintf(stderr, "[fatal] recovery save of the play state: %s\n",
                         engine::core::path_to_utf8(*saved->play).c_str());
        }
    } else {
        std::fprintf(stderr, "[fatal] recovery save failed: %s\n",
                     saved.error().message.c_str());
    }
    std::fprintf(stderr, "[fatal] rendering stopped; exiting with status %d\n",
                 k_exit_gpu_fault);
    std::fflush(stderr);
    return k_exit_gpu_fault;
}

} // namespace quat::app
