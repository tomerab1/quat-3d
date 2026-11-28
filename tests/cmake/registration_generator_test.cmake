# registration_generator (ROADMAP S9): a file dropped into a registration
# directory gets its registration call generated, with no list to edit.
#   cmake -DROOT=<repo> -DWORK=<scratch dir> -P registration_generator_test.cmake
cmake_policy(VERSION 3.22)
include("${ROOT}/cmake/QuatRegistration.cmake")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/modules")
file(WRITE "${WORK}/table.cpp.in"
    "// @QUAT_REG_COUNT@ modules\n@QUAT_REG_DECLARATIONS@@QUAT_REG_CALLS@@QUAT_REG_NAMES@")

function(generate out)
    file(GLOB files "${WORK}/modules/*_commands.cpp")
    quat_write_registration(FILES ${files} TEMPLATE "${WORK}/table.cpp.in"
        OUTPUT "${WORK}/table.cpp"
        DECLARE "void register_@NAME@(Server& server)"
        CALL "    register_@NAME@(server)")
    file(READ "${WORK}/table.cpp" text)
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(expect text needle)
    string(FIND "${text}" "${needle}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "registration_generator: expected\n  ${needle}\nin\n${text}")
    endif()
endfunction()

file(WRITE "${WORK}/modules/zebra_commands.cpp" "")
file(WRITE "${WORK}/modules/alpha_commands.cpp" "")
file(WRITE "${WORK}/modules/notes.txt" "")
generate(text)
expect("${text}" "// 2 modules")
expect("${text}" "void register_alpha_commands(Server& server);\nvoid register_zebra_commands(Server& server);\n")
expect("${text}" "    register_alpha_commands(server);\n    register_zebra_commands(server);\n")
expect("${text}" "    \"alpha_commands\",\n    \"zebra_commands\",\n")

# A new module: one new file, and the table follows.
file(WRITE "${WORK}/modules/middle_commands.cpp" "")
generate(text)
expect("${text}" "// 3 modules")
expect("${text}" "    register_alpha_commands(server);\n    register_middle_commands(server);\n    register_zebra_commands(server);\n")

# Removing the file removes its registration.
file(REMOVE "${WORK}/modules/zebra_commands.cpp")
generate(text)
expect("${text}" "// 2 modules")
string(FIND "${text}" "zebra" at)
if(NOT at EQUAL -1)
    message(FATAL_ERROR "registration_generator: a removed module is still registered:\n${text}")
endif()
message(STATUS "registration_generator: ok")
