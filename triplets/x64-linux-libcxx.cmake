# Custom vcpkg triplet: static libs built with clang + libc++.
#
# Why this exists: the engine itself is compiled clang++ -stdlib=libc++ (only
# that toolchain provides the C++23 library we need — see CMakePresets.json and
# the project memory). Header-only ports (glm, nlohmann-json, VMA) link fine
# against either standard library, but any port with a *compiled* C++ component
# (e.g. fastgltf + its simdjson dependency) must be built with libc++ too, or
# linking the final binary fails with undefined libstdc++/libc++ symbol clashes.
#
# The chainload toolchain forces clang + libc++ for every port build.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_CXX_FLAGS "-stdlib=libc++")
set(VCPKG_C_FLAGS "")
set(VCPKG_LINKER_FLAGS "-stdlib=libc++")

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/toolchains/clang-libcxx.cmake")
