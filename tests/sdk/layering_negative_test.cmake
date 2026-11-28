# layering_check_detects_violation (ROADMAP P8): both halves of layering_check
# fail on a known violation, so a broken pattern cannot turn the check into a
# silent no-op that stays green.
#   cmake -DSOURCE_DIR=<repo> -DBINARY=<scratch dir> -P layering_negative_test.cmake
cmake_policy(VERSION 3.22)
set(fixture "${SOURCE_DIR}/tests/sdk/layering_fixture")

# Text half: an #include of a sample, a relative CMake path into a sample, and
# an installed header that includes an uninstalled one.
execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${fixture}/tree
        -P "${SOURCE_DIR}/cmake/CheckLayering.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(result EQUAL 0)
    message(FATAL_ERROR "CheckLayering.cmake passed a tree with violations:\n${out}")
endif()
foreach(expected
        "src/bad_include.cpp: refers to a sample"
        "tests/CMakeLists.txt: refers to a sample"
        "include/engine/leaky.hpp: includes engine/private_detail.hpp, which the SDK does not install")
    string(FIND "${out}" "${expected}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "CheckLayering.cmake did not report '${expected}':\n${out}")
    endif()
endforeach()

# Link half: an engine target linking a sample target by name.
file(REMOVE_RECURSE "${BINARY}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${fixture}/link" -B "${BINARY}"
        "-DQUAT3D_CHECK=${SOURCE_DIR}/cmake/CheckLinkLayering.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE out)
string(REGEX REPLACE "[ \t\r\n]+" " " folded "${out}")
if(result EQUAL 0 OR NOT folded MATCHES "engine_fixture \\(INTERFACE_LINK_LIBRARIES\\) links sample_fixture_lib")
    message(FATAL_ERROR "CheckLinkLayering.cmake did not refuse an engine target linking a sample:\n${out}")
endif()
string(FIND "${folded}" "Third::party" third)
if(NOT third EQUAL -1)
    message(FATAL_ERROR "CheckLinkLayering.cmake flagged an imported dependency:\n${out}")
endif()
message(STATUS "layering_check refused every fixture violation")
