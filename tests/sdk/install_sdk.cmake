# sdk_install (ROADMAP P8): installs the built engine as an SDK into a fresh
# prefix — the real layout a game gets — and checks that the package files
# and headers name neither the source nor the build tree (relocatable).
#   cmake -DBUILD=<build dir> -DPREFIX=<sdk> -DCONFIG=<config> -DSOURCE_DIR=<repo> -P install_sdk.cmake
cmake_policy(VERSION 3.22)
foreach(var BUILD PREFIX SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "install_sdk.cmake: -D${var}=... is required")
    endif()
endforeach()
if(NOT CONFIG)
    set(CONFIG Release)
endif()

file(REMOVE_RECURSE "${PREFIX}")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${PREFIX}" --config "${CONFIG}"
    RESULT_VARIABLE result OUTPUT_QUIET)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "cmake --install ${BUILD} --prefix ${PREFIX} failed")
endif()

foreach(required
        lib/cmake/quat3d/quat3dConfig.cmake
        lib/cmake/quat3d/quat3dConfigVersion.cmake
        lib/cmake/quat3d/quat3dTargets.cmake
        lib/cmake/quat3d/quat3d-toolchain.cmake
        lib/cmake/quat3d/Quat3dGame.cmake
        include/engine/version.hpp
        include/engine/scene/scene.hpp
        share/quat3d/cooked/shaders
        share/quat3d/assets/shaders/lib/material.slang
        share/quat3d/assets/fonts
        # Licences of code compiled into lib/ that vcpkg's copyright files do
        # not cover (a new FetchContent dependency adds its own here).
        share/quat3d/licenses/acl-LICENSE
        share/quat3d/licenses/rtm-LICENSE
        share/quat3d/licenses/mikktspace-LICENSE
        share/quat3d/licenses/slang-LICENSE
        # quat-3d's own licence, at the root and in quat-pack's engine root,
        # which copies it into every game package (#122).
        LICENSE
        share/quat3d/LICENSE
        # quat-pack's engine root (P4).
        share/quat3d/scripts/package-release.sh
        share/quat3d/scripts/smoke-test-package.sh
        share/quat3d/packaging/game)
    if(NOT EXISTS "${PREFIX}/${required}")
        message(FATAL_ERROR "the installed SDK lacks ${required}")
    endif()
endforeach()
file(GLOB cook "${PREFIX}/bin/quat-cook*")
file(GLOB slangc "${PREFIX}/bin/slangc*")
file(GLOB pack "${PREFIX}/bin/quat-pack*")
file(GLOB player "${PREFIX}/bin/quat-player*")
if(NOT cook OR NOT slangc OR NOT pack OR NOT player)
    message(FATAL_ERROR "the installed SDK lacks one of bin/quat-cook, bin/slangc, bin/quat-pack, bin/quat-player")
endif()
# Private and graphics-API headers stay out of the SDK.
foreach(forbidden engine/rhi engine/cook engine/editor engine/renderer/mesh_pass.hpp
        engine/asset/mesh_asset.hpp engine/asset/material_asset.hpp)
    if(EXISTS "${PREFIX}/include/${forbidden}")
        message(FATAL_ERROR "the SDK installs ${forbidden}, which is not part of the stable API")
    endif()
endforeach()

# Relocatable: no build-machine paths in the package files or headers.
file(GLOB_RECURSE scanned "${PREFIX}/lib/cmake/quat3d/*" "${PREFIX}/include/*")
foreach(file IN LISTS scanned)
    file(READ "${file}" text)
    foreach(path "${SOURCE_DIR}" "${BUILD}")
        string(FIND "${text}" "${path}" at)
        if(NOT at EQUAL -1)
            message(FATAL_ERROR "${file} names the build machine's ${path}: the SDK is not relocatable")
        endif()
    endforeach()
endforeach()
list(LENGTH scanned count)
message(STATUS "SDK installed in ${PREFIX}; ${count} package files and headers are relocatable")
