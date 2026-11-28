# Engine SDK (ROADMAP P8): umbrella targets, install rules and the quat3d CMake
# package. docs/sdk.md is the consumer's guide; docs/design/cozy-builder.md
# §3.3–3.5 the design. Follows Kitware's "Importing and Exporting Guide"
# (cmake.org/cmake/help/latest/guide/importing-exporting) for the exported
# targets, package config and version files; versions follow Semantic
# Versioning 2.0.0 (semver.org), see docs/reading/README.md.
#
#   cmake --build build/release
#   cmake --install build/release --prefix <sdk>
#
# Layout of <sdk> (relocatable: nothing below names the build machine):
#   include/engine/**        the stable header set (cmake/Quat3dSdkHeaders.cmake)
#   lib/                     engine static libraries
#   lib/cmake/quat3d/        quat3dConfig.cmake, quat3dConfigVersion.cmake,
#                            quat3dTargets*.cmake, quat3d-toolchain.cmake,
#                            Quat3dGame.cmake, Quat3dToolchainCheck.cmake
#   bin/                     quat-cook, slangc (+ its libraries), quat-mcp
#   deps/                    the vcpkg installed tree the engine was built with
#                            (release only), so a game never runs vcpkg
#   share/quat3d/            assets/ (engine Slang modules, editor fonts),
#                            cooked/shaders/, licences

include(CMakePackageConfigHelpers)
include(${CMAKE_CURRENT_LIST_DIR}/Quat3dSdkHeaders.cmake)

# ---------------------------------------------------------------------------
# Umbrella targets: what a game links. Exported as quat3d::engine,
# quat3d::remote and quat3d::editor; the ALIASes give an engine source
# checkout (QUAT3D_SOURCE_DIR, docs/sdk.md) the same names.
# ---------------------------------------------------------------------------
# The engine libraries are every engine_* library target of the engine's
# module directories (src/<module>/CMakeLists.txt), derived rather than listed, so a new engine library (engine_weather, ...)
# joins the SDK and quat3d::engine without an edit here; the sdk_consumer and
# public-header guards then check it is installable. Not in the SDK:
# engine_cook (offline cooker, shipped as bin/quat-cook) and the editor
# (exported only with ENGINE_EDITOR, below).
# The root directory and every module directory under src/ (S9: one
# CMakeLists.txt per module), depth-first in add_subdirectory order, which is
# the definition order. Games and samples (games/, examples/) and tests/ are
# never walked: an engine library lives under src/.
function(quat3d_engine_directory_targets dir out)
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subdirs)
        cmake_path(IS_PREFIX PROJECT_SOURCE_DIR "${sub}" NORMALIZE inside_project)
        file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}" "${sub}")
        if(inside_project AND rel MATCHES "^src(/|$)")
            quat3d_engine_directory_targets(${sub} sub_targets)
            list(APPEND targets ${sub_targets})
        endif()
    endforeach()
    set(${out} ${targets} PARENT_SCOPE)
endfunction()
quat3d_engine_directory_targets(${PROJECT_SOURCE_DIR} quat3d_directory_targets)
set(QUAT3D_ENGINE_LIBRARIES)
foreach(target IN LISTS quat3d_directory_targets)
    if(NOT target MATCHES "^engine_" OR target MATCHES "^engine_(cook|editor)$")
        continue()
    endif()
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "INTERFACE_LIBRARY")
        list(APPEND QUAT3D_ENGINE_LIBRARIES ${target})
    endif()
endforeach()
set(quat3d_engine_links ${QUAT3D_ENGINE_LIBRARIES})
list(REMOVE_ITEM quat3d_engine_links engine_flags engine_remote engine_remote_world)

add_library(quat3d_engine INTERFACE)
target_link_libraries(quat3d_engine INTERFACE ${quat3d_engine_links})
set_target_properties(quat3d_engine PROPERTIES EXPORT_NAME engine)
add_library(quat3d::engine ALIAS quat3d_engine)

add_library(quat3d_remote INTERFACE)
target_link_libraries(quat3d_remote INTERFACE quat3d_engine engine_remote engine_remote_world)
set_target_properties(quat3d_remote PROPERTIES EXPORT_NAME remote)
add_library(quat3d::remote ALIAS quat3d_remote)

set(QUAT3D_EXPORTED_TARGETS ${QUAT3D_ENGINE_LIBRARIES} quat3d_engine quat3d_remote)
if(ENGINE_EDITOR)
    add_library(quat3d_editor INTERFACE)
    target_link_libraries(quat3d_editor INTERFACE quat3d_engine engine_editor)
    set_target_properties(quat3d_editor PROPERTIES EXPORT_NAME editor)
    add_library(quat3d::editor ALIAS quat3d_editor)
    list(APPEND QUAT3D_EXPORTED_TARGETS engine_editor quat3d_editor)
endif()
list(JOIN QUAT3D_ENGINE_LIBRARIES " " quat3d_engine_list)
message(STATUS "quat3d SDK engine libraries: ${quat3d_engine_list}")

# layering_check's link-graph half: once every target exists (samples are
# added later), no engine target may link a non-engine local target.
# engine_cook is an engine library outside the SDK; acl_headers is the
# configure-time ACL/RTM helper (cmake/FetchAcl.cmake).
include(${CMAKE_CURRENT_LIST_DIR}/CheckLinkLayering.cmake)
set_property(GLOBAL PROPERTY QUAT3D_LAYERING_ENGINE_TARGETS ${QUAT3D_EXPORTED_TARGETS} engine_cook)
set_property(GLOBAL PROPERTY QUAT3D_LAYERING_ALLOWED_LOCAL acl_headers)
cmake_language(DEFER DIRECTORY ${PROJECT_SOURCE_DIR} CALL quat3d_check_link_layering)

# Quat3dGame.cmake works from a source checkout too (QUAT3D_SOURCE_DIR).
set(QUAT3D_DATA_DIR ${PROJECT_SOURCE_DIR} CACHE INTERNAL "")
set(QUAT3D_SLANGC ${SLANGC_EXECUTABLE} CACHE INTERNAL "")
include(${CMAKE_CURRENT_LIST_DIR}/Quat3dGame.cmake)

# Installing is the top-level build's business; a parent project that
# add_subdirectory()s the engine (QUAT3D_SOURCE_DIR) gets the targets only.
option(QUAT3D_INSTALL "Generate the engine SDK install rules (ROADMAP P8)" ${PROJECT_IS_TOP_LEVEL})
if(NOT QUAT3D_INSTALL)
    return()
endif()

set(QUAT3D_CMAKE_DIR lib/cmake/quat3d)
set(QUAT3D_SHARE_DIR share/quat3d)

# ---------------------------------------------------------------------------
# Libraries, headers and the package files.
# ---------------------------------------------------------------------------
install(TARGETS ${QUAT3D_EXPORTED_TARGETS}
    EXPORT quat3dTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include)

foreach(header IN LISTS QUAT3D_SDK_HEADERS)
    get_filename_component(header_dir ${header} DIRECTORY)
    install(FILES ${PROJECT_SOURCE_DIR}/include/${header} DESTINATION include/${header_dir})
endforeach()
install(FILES ${ENGINE_GENERATED_INCLUDE}/engine/version.hpp DESTINATION include/engine)

install(EXPORT quat3dTargets
    NAMESPACE quat3d::
    DESTINATION ${QUAT3D_CMAKE_DIR})

# Toolchain the SDK was built with: quat3dConfig.cmake refuses another one
# (Quat3dToolchainCheck.cmake), quat3d-toolchain.cmake selects it.
set(QUAT3D_SDK_STDLIB "")
if(MSVC)
    set(QUAT3D_SDK_STDLIB msvc)
elseif(APPLE OR CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+")
    set(QUAT3D_SDK_STDLIB libc++)
else()
    set(QUAT3D_SDK_STDLIB libstdc++)
endif()
get_filename_component(QUAT3D_SDK_CXX_COMPILER_NAME "${CMAKE_CXX_COMPILER}" NAME)
# The matching C driver (clang++-18 -> clang-18), so a game that enables C
# does not mix compiler versions.
string(REPLACE "clang++" "clang" QUAT3D_SDK_C_COMPILER_NAME "${QUAT3D_SDK_CXX_COMPILER_NAME}")

configure_package_config_file(
    ${CMAKE_CURRENT_LIST_DIR}/quat3dConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3dConfig.cmake
    INSTALL_DESTINATION ${QUAT3D_CMAKE_DIR})
# 0.x: a MINOR release may break the API (docs/sdk.md), so a game asking for
# 0.4 must not get 0.5: SameMinorVersion. From 1.0 breaks wait for a MAJOR
# release and SameMajorVersion applies.
if(PROJECT_VERSION_MAJOR EQUAL 0)
    set(quat3d_compatibility SameMinorVersion)
else()
    set(quat3d_compatibility SameMajorVersion)
endif()
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3dConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY ${quat3d_compatibility})
configure_file(${CMAKE_CURRENT_LIST_DIR}/quat3d-toolchain.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3d-toolchain.cmake @ONLY)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3dConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3dConfigVersion.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/sdk/quat3d-toolchain.cmake
    ${CMAKE_CURRENT_LIST_DIR}/Quat3dGame.cmake
    ${CMAKE_CURRENT_LIST_DIR}/Quat3dToolchainCheck.cmake
    DESTINATION ${QUAT3D_CMAKE_DIR})

# ---------------------------------------------------------------------------
# Tools: quat-cook, slangc (with the shared libraries it loads), quat-mcp.
# ---------------------------------------------------------------------------
# quat-pack and the stock players (P4): a data-plus-Lua game runs in
# quat-player and ships with quat-pack. quat-pack finds the engine root it
# packages with (scripts/ + packaging/) and the cooked shaders in
# share/quat3d (engine::pack::apply_installed_sdk_defaults).
set(quat3d_sdk_programs quat-cook)
foreach(program quat-pack quat-player quat-player-shipping)
    if(TARGET ${program})
        list(APPEND quat3d_sdk_programs ${program})
    endif()
endforeach()
install(TARGETS ${quat3d_sdk_programs} RUNTIME DESTINATION bin BUNDLE DESTINATION bin)
if(WIN32)
    # The x64-windows triplet links dependencies as DLLs.
    foreach(program IN LISTS quat3d_sdk_programs)
        install(FILES $<TARGET_RUNTIME_DLLS:${program}> DESTINATION bin)
    endforeach()
endif()

# Slang's release layout is bin/slangc + lib/ (shared libraries found through
# slangc's RPATH, $ORIGIN/../lib) on Linux and macOS, everything in bin/ on
# Windows. slang-llvm (CPU targets, ~100 MB) is not needed for SPIR-V.
get_filename_component(quat3d_slang_bin "${SLANGC_EXECUTABLE}" REALPATH)
get_filename_component(quat3d_slang_bin "${quat3d_slang_bin}" DIRECTORY)
get_filename_component(quat3d_slang_root "${quat3d_slang_bin}" DIRECTORY)
install(PROGRAMS ${SLANGC_EXECUTABLE} DESTINATION bin)
if(WIN32)
    install(DIRECTORY ${quat3d_slang_bin}/ DESTINATION bin
        FILES_MATCHING PATTERN "*.dll" PATTERN "slang-llvm*" EXCLUDE)
elseif(EXISTS ${quat3d_slang_root}/lib)
    install(DIRECTORY ${quat3d_slang_root}/lib/ DESTINATION lib
        USE_SOURCE_PERMISSIONS
        FILES_MATCHING PATTERN "*.so*" PATTERN "*.dylib"
        PATTERN "*slang-llvm*" EXCLUDE PATTERN "cmake" EXCLUDE)
endif()
if(EXISTS ${quat3d_slang_root}/LICENSE)
    install(FILES ${quat3d_slang_root}/LICENSE
        DESTINATION ${QUAT3D_SHARE_DIR}/licenses RENAME slang-LICENSE)
endif()

# quat-mcp is a cargo build (tools/quat-mcp); the release workflow builds it
# before installing. Skipped when it was not built.
set(QUAT3D_SDK_QUAT_MCP
    "${PROJECT_SOURCE_DIR}/tools/quat-mcp/target/release/quat-mcp${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE FILEPATH "quat-mcp executable installed into the SDK's bin/ (when it exists)")
install(PROGRAMS ${QUAT3D_SDK_QUAT_MCP} DESTINATION bin OPTIONAL)

# ---------------------------------------------------------------------------
# Data: share/quat3d is a data root (assets/ + cooked/shaders/, see
# engine/core/asset_root.hpp): the engine's cooked shaders, the Slang modules
# game material shaders import, and the editor's fonts and icons.
# ---------------------------------------------------------------------------
install(DIRECTORY ${PROJECT_SOURCE_DIR}/cooked/shaders/
    DESTINATION ${QUAT3D_SHARE_DIR}/cooked/shaders)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/shaders/lib/
    DESTINATION ${QUAT3D_SHARE_DIR}/assets/shaders/lib
    FILES_MATCHING PATTERN "*.slang")
install(DIRECTORY ${PROJECT_SOURCE_DIR}/assets/fonts/
    DESTINATION ${QUAT3D_SHARE_DIR}/assets/fonts)
# Licences of code compiled into the SDK's libraries that does not come
# through vcpkg (deps/share/<port>/copyright covers those): ACL and RTM are
# header-only FetchContent dependencies built into libengine_animation
# (cmake/FetchAcl.cmake). sdk_install requires every file named here.
install(FILES ${acl_SOURCE_DIR}/LICENSE DESTINATION ${QUAT3D_SHARE_DIR}/licenses RENAME acl-LICENSE)
install(FILES ${rtm_SOURCE_DIR}/LICENSE DESTINATION ${QUAT3D_SHARE_DIR}/licenses RENAME rtm-LICENSE)
# quat-pack's engine root (P4): the per-OS package layout script, the smoke
# test it runs, and the README / Info.plist templates it fills in.
install(PROGRAMS
    ${PROJECT_SOURCE_DIR}/scripts/package-release.sh
    ${PROJECT_SOURCE_DIR}/scripts/smoke-test-package.sh
    DESTINATION ${QUAT3D_SHARE_DIR}/scripts)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/packaging/ DESTINATION ${QUAT3D_SHARE_DIR}/packaging)
# The engine's LICENSE (#96), at the SDK root and in share/quat3d, the engine
# root quat-pack copies it from into every game package (#122;
# find_game_notices in scripts/package-release.sh). The full
# THIRD_PARTY_NOTICES.md files are generated from the build when the SDK is
# packaged (scripts/package-sdk.sh): the repository's copy is only the summary
# table, which quat-pack rightly ignores.
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION ${QUAT3D_SHARE_DIR})

# ---------------------------------------------------------------------------
# deps/: the vcpkg installed tree for this triplet, release half only (the SDK
# is a release build; quat3d-toolchain.cmake maps every consumer configuration
# onto it). Debug imports are dropped with the debug libraries they name.
# ---------------------------------------------------------------------------
option(QUAT3D_SDK_BUNDLE_DEPS "Install the vcpkg dependency tree into the SDK's deps/" ON)
if(QUAT3D_SDK_BUNDLE_DEPS AND DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(quat3d_vcpkg_tree "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    foreach(sub include lib share bin)
        if(EXISTS "${quat3d_vcpkg_tree}/${sub}")
            install(DIRECTORY "${quat3d_vcpkg_tree}/${sub}" DESTINATION deps
                USE_SOURCE_PERMISSIONS
                PATTERN "*-debug.cmake" EXCLUDE
                PATTERN "*.pdb" EXCLUDE)
        endif()
    endforeach()
endif()
