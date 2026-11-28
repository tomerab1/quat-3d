# Toolchain match for the engine SDK (ROADMAP P8, docs/sdk.md "Toolchain").
#
# The SDK is static C++ with no ABI promise across compilers or standard
# libraries, so a game must use the toolchain family the SDK was built with:
# clang + libc++ on Linux, MSVC v14x on Windows, Apple clang on macOS.
# quat3dConfig.cmake calls quat3d_toolchain_problem() and refuses a mismatch
# with a message naming the expected compiler, instead of failing later with
# link errors. The function is pure (no try_compile), so the rule is tested
# in script mode: tests/sdk/toolchain_check_test.cmake.

# Sets <out> to an empty string when the consumer toolchain matches the SDK's,
# else to a one-paragraph explanation.
#   sdk_id / sdk_version / sdk_stdlib: CMAKE_CXX_COMPILER_ID, _VERSION and the
#       standard library (libc++, libstdc++, msvc) of the SDK build.
#   id / version / stdlib: the same for the consuming project.
function(quat3d_toolchain_problem out sdk_id sdk_version sdk_stdlib id version stdlib)
    set(problem "")
    string(REGEX MATCH "^[0-9]+" sdk_major "${sdk_version}")
    string(REGEX MATCH "^[0-9]+" major "${version}")
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" sdk_major_minor "${sdk_version}")
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" major_minor "${version}")
    if(NOT id STREQUAL sdk_id)
        set(problem "it uses ${id} ${version}, and the SDK was built with ${sdk_id} ${sdk_version}")
    elseif(id STREQUAL "MSVC")
        # Static libraries need a linker and runtime at least as new as the
        # newest compiler that built them; v14x toolsets are compatible.
        if(NOT major STREQUAL sdk_major OR major_minor VERSION_LESS sdk_major_minor)
            set(problem "it uses MSVC ${version}; the SDK needs MSVC ${sdk_major_minor} or newer in the same v14x toolset family")
        endif()
    elseif(NOT major STREQUAL sdk_major)
        set(problem "it uses ${id} ${version}, and the SDK was built with ${sdk_id} ${sdk_version} (the major versions must match)")
    endif()
    if(problem STREQUAL "" AND NOT stdlib STREQUAL sdk_stdlib)
        set(problem "it compiles against ${stdlib}, and the SDK was built against ${sdk_stdlib}")
    endif()
    set(${out} "${problem}" PARENT_SCOPE)
endfunction()
