// Driver identification logged at device creation (issue #130): which
// driver loaded and which loader override, if any, picked it.

#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include "engine/rhi/device.hpp"
#include "engine/rhi/driver_info.hpp"
#include "support/gpu_context.hpp"

namespace {

namespace rhi = engine::rhi;

rhi::GetEnvFn fake_env(const std::map<std::string, std::string>& vars) {
    return [vars](const char* name) -> const char* {
        const auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
    };
}

rhi::DriverInfo moltenvk() {
    rhi::DriverInfo d;
    d.device_name = "Apple M2";
    d.driver_name = "MoltenVK";
    d.driver_info = "1.4.1";
    d.driver_id = VK_DRIVER_ID_MOLTENVK;
    d.api_version = VK_MAKE_API_VERSION(0, 1, 4, 334);
    d.driver_version = 0x2711U;
    return d;
}

TEST(DriverInfo, ReportsTheLoaderDriverVariablesThatAreSet) {
    const auto overrides = rhi::loader_driver_overrides(
        fake_env({{"VK_ICD_FILENAMES", "/sdk/MoltenVK_icd.json"},
                  {"VK_ADD_DRIVER_FILES", ""}, // empty = unset
                  {"VK_DRIVER_FILES", "/sdk/a.json:/sdk/b.json"},
                  {"VK_LAYER_PATH", "/sdk/layers"}})); // not a driver variable
    ASSERT_EQ(overrides.size(), 2U);
    EXPECT_EQ(overrides[0].variable, "VK_DRIVER_FILES");
    EXPECT_EQ(overrides[0].value, "/sdk/a.json:/sdk/b.json");
    EXPECT_EQ(overrides[1].variable, "VK_ICD_FILENAMES");
    EXPECT_TRUE(rhi::loader_driver_overrides(fake_env({})).empty());
    EXPECT_TRUE(rhi::loader_driver_overrides(nullptr).empty());
}

TEST(DriverInfo, DescribesTheDriverVersionAndWhereTheLoaderLooked) {
    const std::string bundled = rhi::describe_driver(moltenvk(), {});
    EXPECT_NE(bundled.find("MoltenVK 1.4.1"), std::string::npos) << bundled;
    EXPECT_NE(bundled.find("Vulkan 1.4.334"), std::string::npos) << bundled;
    EXPECT_NE(bundled.find("Apple M2"), std::string::npos) << bundled;
    EXPECT_NE(bundled.find("loader default discovery"), std::string::npos) << bundled;

    const std::string sdk = rhi::describe_driver(
        moltenvk(), {{"VK_ICD_FILENAMES", "/Users/x/VulkanSDK/MoltenVK_icd.json"}});
    EXPECT_NE(sdk.find("VK_ICD_FILENAMES=/Users/x/VulkanSDK/MoltenVK_icd.json"),
              std::string::npos)
        << sdk;
    EXPECT_EQ(sdk.find("default discovery"), std::string::npos) << sdk;

    const std::string blank = rhi::describe_driver(rhi::DriverInfo{}, {});
    EXPECT_NE(blank.find("<unnamed>"), std::string::npos) << blank;
}

// The selected device's driver is queried at creation (every conformant
// Vulkan 1.2+ driver names itself).
TEST(GpuDriverInfo, DeviceReportsItsDriver) {
    QUAT_REQUIRE_GPU(g);
    const rhi::DriverInfo& d = g->device.driver();
    EXPECT_FALSE(d.driver_name.empty());
    EXPECT_FALSE(d.device_name.empty());
    EXPECT_NE(static_cast<std::uint32_t>(d.driver_id), 0U);
    EXPECT_EQ(d.api_version, g->device.properties().apiVersion);
}

} // namespace
