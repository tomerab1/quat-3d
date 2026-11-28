# Generated registration tables (ROADMAP S9): one source file per registered
# item (a codec component, a remote command module, an inspector), and a
# generated translation unit that calls each file's registration function.
# A new item is a new file in its directory: no central list to edit, so
# parallel feature branches stop conflicting on shared tables.
#
#   quat_generate_registration(<target>
#       DIRECTORY <dir>          # scanned with CONFIGURE_DEPENDS
#       PATTERN   <glob>         # e.g. "*.cpp", "*_commands.cpp"
#       TEMPLATE  <file.cpp.in>  # the generated TU, with the @QUAT_REG_*@ slots below
#       OUTPUT    <file.cpp>
#       DECLARE   <declaration>  # per item, without the ';' (CMake splits
#       CALL      <statement>)   # arguments on it); @NAME@ is the file's stem
#
# adds the matched files and the generated TU to <target>. Items are sorted by
# file name, so the output is deterministic. The template sees
#   @QUAT_REG_DECLARATIONS@  one DECLARE line per item
#   @QUAT_REG_CALLS@         one CALL line per item
#   @QUAT_REG_NAMES@         "stem", per item (a string_view initializer list)
#   @QUAT_REG_COUNT@         the number of items
# Registration is explicit generated code, not static initialisers: a linker
# drops unreferenced objects from static libraries, and the generated TU
# references every item.
#
# quat_write_registration(...) takes FILES instead of TARGET / DIRECTORY /
# PATTERN and only writes OUTPUT; it works in script mode (cmake -P), which the
# registration_generator test uses.
include_guard(GLOBAL)

function(quat_write_registration)
    cmake_parse_arguments(ARG "" "TEMPLATE;OUTPUT;DECLARE;CALL" "FILES" ${ARGN})
    foreach(required TEMPLATE OUTPUT DECLARE CALL)
        if(NOT ARG_${required})
            message(FATAL_ERROR "quat_write_registration: ${required} is required")
        endif()
    endforeach()
    set(names)
    foreach(file IN LISTS ARG_FILES)
        get_filename_component(stem "${file}" NAME_WE)
        if(NOT stem MATCHES "^[a-z][a-z0-9_]*$")
            message(FATAL_ERROR "quat_write_registration: ${file}: a registered file's name must be "
                "lowercase letters, digits and '_' (it becomes a C++ identifier)")
        endif()
        list(APPEND names ${stem})
    endforeach()
    list(SORT names)
    list(REMOVE_DUPLICATES names)
    set(QUAT_REG_DECLARATIONS "")
    set(QUAT_REG_CALLS "")
    set(QUAT_REG_NAMES "")
    list(LENGTH names QUAT_REG_COUNT)
    string(ASCII 59 semicolon)
    foreach(name IN LISTS names)
        string(REPLACE "@NAME@" "${name}" declare "${ARG_DECLARE}")
        string(REPLACE "@NAME@" "${name}" call "${ARG_CALL}")
        string(APPEND QUAT_REG_DECLARATIONS "${declare}${semicolon}\n")
        string(APPEND QUAT_REG_CALLS "${call}${semicolon}\n")
        string(APPEND QUAT_REG_NAMES "    \"${name}\",\n")
    endforeach()
    configure_file("${ARG_TEMPLATE}" "${ARG_OUTPUT}" @ONLY)
endfunction()

function(quat_generate_registration target)
    cmake_parse_arguments(ARG "" "DIRECTORY;PATTERN;TEMPLATE;OUTPUT;DECLARE;CALL" "" ${ARGN})
    file(GLOB files CONFIGURE_DEPENDS "${ARG_DIRECTORY}/${ARG_PATTERN}")
    quat_write_registration(FILES ${files} TEMPLATE "${ARG_TEMPLATE}" OUTPUT "${ARG_OUTPUT}"
        DECLARE "${ARG_DECLARE}" CALL "${ARG_CALL}")
    target_sources(${target} PRIVATE ${files} "${ARG_OUTPUT}")
    # The generated TU lives in the build tree: it reaches the module's
    # private headers as "<module>/<header>" through src/.
    set_source_files_properties("${ARG_OUTPUT}" PROPERTIES
        INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/src")
endfunction()
