# Engine SDK guards (ROADMAP P8, docs/sdk.md). Label `sdk`:
#   sdk_install                   install the build into a fresh prefix (fixture)
#   public_headers_self_contained every installed header compiles alone
#   sdk_consumer                  examples/sdk-consumer builds + tests against it
#   sdk_config_refusal_version    find_package(quat3d <other minor>) is refused
#   sdk_config_refusal_compiler   a mismatched compiler is refused (Linux: g++)
#   sdk_toolchain_rules           the compiler-match rule, in script mode
#   layering_check                no engine file depends on a game or sample
#   layering_check_detects_violation  both layering halves fail on fixture violations
set(sdk_test_dir ${CMAKE_BINARY_DIR}/sdk-test)
set(sdk_prefix ${sdk_test_dir}/prefix)
set(run_project ${CMAKE_CURRENT_SOURCE_DIR}/run_project.cmake)
set(project_args
    -DPREFIX=${sdk_prefix}
    "-DGENERATOR=${CMAKE_GENERATOR}"
    -DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM})

add_test(NAME sdk_install
    COMMAND ${CMAKE_COMMAND} -DBUILD=${CMAKE_BINARY_DIR} -DPREFIX=${sdk_prefix}
            -DCONFIG=$<CONFIG> -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/install_sdk.cmake)
set_tests_properties(sdk_install PROPERTIES FIXTURES_SETUP quat3d_sdk)

add_test(NAME public_headers_self_contained
    COMMAND ${CMAKE_COMMAND} ${project_args}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/headers -DBINARY=${sdk_test_dir}/headers
            -DARGS=-DQUAT3D_PREFIX=${sdk_prefix}
            -P ${run_project})
add_test(NAME sdk_consumer
    COMMAND ${CMAKE_COMMAND} ${project_args}
            -DSOURCE=${PROJECT_SOURCE_DIR}/examples/sdk-consumer -DBINARY=${sdk_test_dir}/consumer
            -DRUN_CTEST=ON
            -P ${run_project})
# 0.x: SameMinorVersion — another minor is a different API.
math(EXPR other_minor "${PROJECT_VERSION_MINOR} + 1")
add_test(NAME sdk_config_refusal_version
    COMMAND ${CMAKE_COMMAND} ${project_args}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/probe -DBINARY=${sdk_test_dir}/probe-version
            -DARGS=-DPROBE_VERSION=${PROJECT_VERSION_MAJOR}.${other_minor}
            "-DEXPECT_FAILURE=compatible with requested version .?${PROJECT_VERSION_MAJOR}[.]${other_minor}"
            -P ${run_project})
set(sdk_tests public_headers_self_contained sdk_consumer sdk_config_refusal_version)
# A genuinely different compiler: GCC on the Linux runner (on macOS `g++` is
# Apple clang, on Windows the SDK is MSVC-only).
find_program(QUAT3D_TEST_GXX g++)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND QUAT3D_TEST_GXX AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_test(NAME sdk_config_refusal_compiler
        COMMAND ${CMAKE_COMMAND} ${project_args}
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/probe -DBINARY=${sdk_test_dir}/probe-compiler
                "-DARGS=-DPROBE_VERSION=${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}|-DCMAKE_CXX_COMPILER=${QUAT3D_TEST_GXX}"
                "-DEXPECT_FAILURE=cannot be used by this project's toolchain: it uses GNU"
                -P ${run_project})
    list(APPEND sdk_tests sdk_config_refusal_compiler)
endif()
set_tests_properties(${sdk_tests} PROPERTIES FIXTURES_REQUIRED quat3d_sdk TIMEOUT 1800)

add_test(NAME sdk_toolchain_rules
    COMMAND ${CMAKE_COMMAND} -DCHECK=${PROJECT_SOURCE_DIR}/cmake/Quat3dToolchainCheck.cmake
            -P ${CMAKE_CURRENT_SOURCE_DIR}/toolchain_check_test.cmake)
add_test(NAME layering_check
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -P ${PROJECT_SOURCE_DIR}/cmake/CheckLayering.cmake)
add_test(NAME layering_check_detects_violation
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DBINARY=${sdk_test_dir}/layering-link
            -P ${CMAKE_CURRENT_SOURCE_DIR}/layering_negative_test.cmake)
set_tests_properties(sdk_install ${sdk_tests} sdk_toolchain_rules layering_check
    layering_check_detects_violation PROPERTIES LABELS sdk)
