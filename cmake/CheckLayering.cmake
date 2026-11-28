# layering_check (ROADMAP P8; docs/design/cozy-builder.md §3.1): the engine
# never depends on a game or a sample.
#   cmake -DROOT=<repo> -P CheckLayering.cmake
# Fails when
#   - a file under src/ include/ tools/ tests/ assets/ cmake/ refers to games/
#     or examples/ in an #include, a CMake path or a Lua require (the root
#     CMakeLists.txt's option-guarded add_subdirectory lines are the only
#     allowed references, and tests/sdk/, which builds examples/sdk-consumer
#     against the installed SDK, the way a game does);
#   - an installed public header (cmake/Quat3dSdkHeaders.cmake) includes an
#     engine header that is not installed, a private src/ path, or a
#     graphics-API header.
cmake_policy(VERSION 3.22)
if(NOT ROOT)
    message(FATAL_ERROR "usage: cmake -DROOT=<repo> -P CheckLayering.cmake")
endif()
set(problems)

# --- 1. No engine file refers to games/ or examples/ ------------------------
set(patterns
    "#[ \t]*include[ \t]*[<\"][^>\"]*(games|examples)/"
    "require[ \t]*\\(?[ \t]*[\"'][^\"']*(games|examples)[./]"
    "(\\$\\{[A-Za-z_]*(SOURCE|PROJECT)_DIR\\}|\\.\\.)/(games|examples)/")
foreach(dir src include tools tests assets cmake)
    file(GLOB_RECURSE files "${ROOT}/${dir}/*.cpp" "${ROOT}/${dir}/*.hpp" "${ROOT}/${dir}/*.h"
        "${ROOT}/${dir}/*.lua" "${ROOT}/${dir}/*.cmake" "${ROOT}/${dir}/CMakeLists.txt"
        "${ROOT}/${dir}/*.rs" "${ROOT}/${dir}/*.slang")
    foreach(file IN LISTS files)
        file(RELATIVE_PATH rel "${ROOT}" "${file}")
        if(rel MATCHES "^tests/sdk/" OR rel MATCHES "^cmake/CheckLayering.cmake$"
           OR rel MATCHES "/target/")
            continue()
        endif()
        file(STRINGS "${file}" lines)
        foreach(pattern IN LISTS patterns)
            foreach(line IN LISTS lines)
                if(line MATCHES "${pattern}")
                    list(APPEND problems "${rel}: refers to a sample: ${line}")
                endif()
            endforeach()
        endforeach()
    endforeach()
endforeach()
file(STRINGS "${ROOT}/CMakeLists.txt" lines)
foreach(line IN LISTS lines)
    if(line MATCHES "(games|examples)/" AND NOT line MATCHES "^[ \t]*add_subdirectory\\((games|examples)/"
       AND NOT line MATCHES "^[ \t]*#")
        list(APPEND problems "CMakeLists.txt: refers to a sample outside an add_subdirectory line: ${line}")
    endif()
endforeach()

# --- 2. Installed headers include only installed headers -----------------------
include("${ROOT}/cmake/Quat3dSdkHeaders.cmake")
set(installed ${QUAT3D_SDK_HEADERS} engine/version.hpp)
foreach(header IN LISTS QUAT3D_SDK_HEADERS)
    if(NOT EXISTS "${ROOT}/include/${header}")
        list(APPEND problems "cmake/Quat3dSdkHeaders.cmake lists ${header}, which does not exist")
        continue()
    endif()
    file(STRINGS "${ROOT}/include/${header}" includes REGEX "^[ \t]*#[ \t]*include")
    foreach(line IN LISTS includes)
        if(NOT line MATCHES "#[ \t]*include[ \t]*[<\"]([^>\"]+)[>\"]")
            continue()
        endif()
        set(target "${CMAKE_MATCH_1}")
        if(target MATCHES "^engine/" AND NOT target IN_LIST installed)
            list(APPEND problems "include/${header}: includes ${target}, which the SDK does not install")
        elseif(target MATCHES "^(src/|\\.\\./)" OR target MATCHES "^(vulkan/|vk_mem_alloc|Jolt/|SDL3/|imgui|sol/|lua)")
            list(APPEND problems "include/${header}: includes ${target} (private or not an SDK dependency)")
        endif()
    endforeach()
endforeach()

if(problems)
    list(LENGTH problems count)
    string(REPLACE ";" "\n  " text "${problems}")
    message(FATAL_ERROR "layering_check: ${count} problem(s):\n  ${text}")
endif()
message(STATUS "layering_check: engine sources are free of samples; ${CMAKE_CURRENT_LIST_FILE} ok")
