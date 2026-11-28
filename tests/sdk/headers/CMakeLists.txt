# public_headers_self_contained (ROADMAP P8): one translation unit per header
# in the INSTALLED include tree, each including only that header (twice, so a
# missing #pragma once fails too). The include path is the installed tree plus
# the SDK's documented public-header dependencies (glm, EnTT, nlohmann_json)
# and nothing else: a header that needs another one first, a private src/
# path, a header left out of the installed set or an undeclared third-party
# include (Vulkan, VMA, Jolt, ...) fails to compile. Warnings are errors.
cmake_minimum_required(VERSION 3.22)
project(quat3d_public_headers LANGUAGES CXX)

if(NOT QUAT3D_PREFIX OR NOT EXISTS "${QUAT3D_PREFIX}/include/engine")
    message(FATAL_ERROR "-DQUAT3D_PREFIX=<installed SDK> is required")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(glm CONFIG REQUIRED)
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

file(GLOB_RECURSE headers RELATIVE "${QUAT3D_PREFIX}/include"
    "${QUAT3D_PREFIX}/include/*.hpp" "${QUAT3D_PREFIX}/include/*.h")
list(LENGTH headers count)
if(count EQUAL 0)
    message(FATAL_ERROR "no headers under ${QUAT3D_PREFIX}/include")
endif()
set(units)
foreach(header IN LISTS headers)
    string(MAKE_C_IDENTIFIER "${header}" unit)
    set(unit "${CMAKE_CURRENT_BINARY_DIR}/units/${unit}.cpp")
    file(WRITE "${unit}" "#include \"${header}\"\n#include \"${header}\"\n")
    list(APPEND units "${unit}")
endforeach()
message(STATUS "public headers: ${count}")

add_library(public_headers OBJECT ${units})
target_include_directories(public_headers PRIVATE "${QUAT3D_PREFIX}/include")
target_link_libraries(public_headers PRIVATE glm::glm EnTT::EnTT nlohmann_json::nlohmann_json)
# The ABI-relevant settings the SDK's exported engine_flags carries.
target_compile_definitions(public_headers PRIVATE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS)
if(MSVC)
    target_compile_options(public_headers PRIVATE /W4 /WX /permissive- /EHs-c- /GR-)
    target_compile_definitions(public_headers PRIVATE _HAS_EXCEPTIONS=0)
else()
    target_compile_options(public_headers PRIVATE
        -Wall -Wextra -Wshadow -Werror -fno-exceptions -fno-rtti)
endif()
