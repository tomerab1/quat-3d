# Custom vcpkg triplet: Apple Silicon, static libraries, macOS 14 minimum.
#
# vcpkg's stock arm64-osx triplet leaves the deployment target at the build
# machine's SDK default, so binaries built on a newer runner refuse to start on
# older macOS (and the linker warns when the engine targets an older version
# than its dependencies). Pinning it here keeps every port and the engine
# (CMAKE_OSX_DEPLOYMENT_TARGET in the macos-release preset) on one minimum,
# which the release .app declares as LSMinimumSystemVersion. Apple clang uses
# libc++ by default, so no toolchain override is needed (unlike Linux).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 14.0)
