#include "engine/version.hpp"

namespace quat3d {

Version version() noexcept {
    return Version{QUAT3D_VERSION_MAJOR, QUAT3D_VERSION_MINOR, QUAT3D_VERSION_PATCH};
}

const char* version_string() noexcept { return QUAT3D_VERSION_STRING; }

} // namespace quat3d
