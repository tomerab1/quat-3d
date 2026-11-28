# Configures, builds and (optionally) tests a CMake project against an
# installed engine SDK, the way a game does (ROADMAP P8). Used by the sdk_*
# CTests (tests/sdk/CMakeLists.txt); the project never sees the source tree.
#
#   cmake -DSOURCE=<project> -DBINARY=<build dir> -DPREFIX=<sdk> -DGENERATOR=<gen>
#         [-DMAKE_PROGRAM=<ninja>] [-DRUN_CTEST=ON] [-DARGS=<-D...|-D...>]
#         [-DEXPECT_FAILURE=<regex>] -P run_project.cmake
#
# EXPECT_FAILURE: the configure step must fail and its output match <regex>
# (refusal tests: wrong version, mismatched compiler).

cmake_policy(VERSION 3.22)
foreach(var SOURCE BINARY PREFIX GENERATOR)
    if(NOT ${var})
        message(FATAL_ERROR "run_project.cmake: -D${var}=... is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${BINARY}")
set(configure "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${BINARY}" -G "${GENERATOR}"
    -DCMAKE_BUILD_TYPE=Release
    "-DCMAKE_PREFIX_PATH=${PREFIX}"
    "-DCMAKE_TOOLCHAIN_FILE=${PREFIX}/lib/cmake/quat3d/quat3d-toolchain.cmake")
if(MAKE_PROGRAM)
    list(APPEND configure "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}")
endif()
# ARGS separates its -D options with '|' (a ';' would split the CTest command).
string(REPLACE "|" ";" ARGS "${ARGS}")
list(APPEND configure ${ARGS})

if(EXPECT_FAILURE)
    execute_process(COMMAND ${configure} RESULT_VARIABLE result
        OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(result EQUAL 0)
        message(FATAL_ERROR "configuring ${SOURCE} succeeded; expected a refusal matching '${EXPECT_FAILURE}'\n${out}")
    endif()
    # CMake wraps long messages: compare with the line breaks folded.
    string(REGEX REPLACE "[ \t\r\n]+" " " folded "${out}")
    if(NOT folded MATCHES "${EXPECT_FAILURE}")
        message(FATAL_ERROR "configuring ${SOURCE} failed, but not with '${EXPECT_FAILURE}':\n${out}")
    endif()
    message(STATUS "refused as expected: ${CMAKE_MATCH_0}")
    return()
endif()

execute_process(COMMAND ${configure} RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "configuring ${SOURCE} against ${PREFIX} failed")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY}" --config Release RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "building ${SOURCE} against ${PREFIX} failed")
endif()
if(RUN_CTEST)
    execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${BINARY}" -C Release --output-on-failure
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "the tests of ${SOURCE} failed")
    endif()
endif()
