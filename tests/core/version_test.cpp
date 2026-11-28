#include <format>
#include <string>

#include <gtest/gtest.h>

#include "engine/version.hpp"

namespace {

// The libraries' version (compiled into engine_core) matches the headers the
// test was compiled against: an SDK whose lib/ and include/ disagree fails.
TEST(Version, LibraryMatchesHeaders) {
    const quat3d::Version v = quat3d::version();
    EXPECT_EQ(v.major, QUAT3D_VERSION_MAJOR);
    EXPECT_EQ(v.minor, QUAT3D_VERSION_MINOR);
    EXPECT_EQ(v.patch, QUAT3D_VERSION_PATCH);
    EXPECT_EQ(v, (quat3d::Version{QUAT3D_VERSION_MAJOR, QUAT3D_VERSION_MINOR,
                                  QUAT3D_VERSION_PATCH}));
}

TEST(Version, StringIsMajorMinorPatch) {
    const quat3d::Version v = quat3d::version();
    EXPECT_EQ(std::string(quat3d::version_string()),
              std::format("{}.{}.{}", v.major, v.minor, v.patch));
    EXPECT_STREQ(quat3d::version_string(), QUAT3D_VERSION_STRING);
}

} // namespace
