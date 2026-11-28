# Overlay of vcpkg's tracy port (same release), client library only, built
# with TRACY_ONLY_LOCALHOST: the profiled game listens for the Tracy viewer on
# the loopback interface only, like the engine's remote-control endpoint
# (profile a remote machine through an SSH tunnel). Upstream's patches only
# concern the viewer / command-line tools, which this port does not build —
# run the Tracy viewer from its release download or a separate build.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO wolfpld/tracy
    REF "v${VERSION}"
    SHA512 18c0c589a1d97d0760958c8ab00ba2135bc602fd359d48445b5d8ed76e5b08742d818bb8f835b599149030f455e553a92db86fb7bae049b47820e4401cf9f935
    HEAD_REF master
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        on-demand TRACY_ON_DEMAND
    INVERTED_FEATURES
        crash-handler TRACY_NO_CRASH_HANDLER
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DTRACY_ONLY_LOCALHOST=ON
        -DDOWNLOAD_CAPSTONE=OFF
        -DLEGACY=ON
        -DCMAKE_DISABLE_FIND_PACKAGE_Git=ON
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        DOWNLOAD_CAPSTONE
        LEGACY
        CMAKE_DISABLE_FIND_PACKAGE_Git
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME Tracy CONFIG_PATH "lib/cmake/Tracy")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
