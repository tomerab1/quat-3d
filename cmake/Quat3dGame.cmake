# quat_add_game() — build a game executable against the engine SDK (ROADMAP
# P8; installed in <sdk>/lib/cmake/quat3d and included by quat3dConfig.cmake).
#
# Native game modules (ROADMAP G17.1, docs/game-modules.md):
#
#   quat_add_game_module(<target>
#       SOURCES <file>...            # the module's C++ sources
#       REGISTER <ns::function>      # its engine::game::RegisterFn
#       [NAME <name>])               # game.json's "modules" name; default:
#                                    # <target> without a trailing "_module"
#
# makes a static library linked to quat3d::engine. quat_add_game(... MODULES)
# then builds the game's executables with a generated main() that calls each
# module's REGISTER function (explicit registration: no plugin DLLs, no static
# initialisers) and hands them to engine::game::run_player / run_editor:
#   <target>             the player              (quat3d::player)
#   <target>-shipping    the shipping player     (quat3d::player_shipping;
#                        remote control compiled out, for quat-pack --player)
#   <target>-editor      the editor with the     (quat3d::player_editor; SDKs
#                        game's modules           built with the editor only)
# PROJECT_DIR is the game project (holds game.json) the executables open when
# QUAT_GAME is not set (not in distribution builds: set QUAT_GAME_PACKAGE_BUILD
# there); SOURCES are then optional extra sources.
#
#   quat_add_game(<target>
#       SOURCES <file>...            # the game's C++ sources (with main()),
#                                    # unless MODULES / PROJECT_DIR generate it
#       [PROJECT_DIR <dir>]          # G17.1: the game project (game.json)
#       [MODULES <module target>...] # G17.1: quat_add_game_module targets
#       [ASSETS <dir>]               # the game's assets/ tree
#       [MATERIALS <dir>]            # authored material shaders (*.slang, ROADMAP A6)
#       [LIBRARIES <target>...]      # extra libraries to link
#       [REMOTE]                     # link quat3d::remote (JSON-RPC endpoint)
#       [EDITOR])                    # link quat3d::editor
#
# The executable's directory becomes a data root (engine/core/asset_root.hpp):
#   <exe dir>/cooked/shaders/            the SDK's cooked engine shaders
#   <exe dir>/cooked/shaders/materials/  MATERIALS, cooked with the SDK's slangc
#   <exe dir>/assets/fonts/              the SDK's editor fonts and icons
#   <exe dir>/assets/...                 ASSETS
# On Windows the dependency DLLs are copied next to the executable
# (quat_copy_runtime_dlls(), below, does the same for test executables).
#
# QUAT3D_DATA_DIR and QUAT3D_SLANGC come from quat3dConfig.cmake (installed
# SDK) or cmake/Quat3dInstall.cmake (engine source checkout).

include_guard(GLOBAL)

set(_QUAT3D_GAME_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Material shaders are linked against the engine's pass-variant driver modules
# exactly as cmake/CompileShaders.cmake cooks the engine's own materials (keep
# the flags in sync with it and with ShaderCompiler::arguments).
function(_quat_cook_materials target dir)
    file(GLOB sources CONFIGURE_DEPENDS ${dir}/*.slang)
    set(lib_dir ${QUAT3D_DATA_DIR}/assets/shaders/lib)
    set(module_dir ${QUAT3D_DATA_DIR}/cooked/shaders)
    set(out_dir ${CMAKE_CURRENT_BINARY_DIR}/${target}-materials)
    set(outputs)
    foreach(src IN LISTS sources)
        get_filename_component(name ${src} NAME_WE)
        foreach(variant gbuffer)
            set(spv ${out_dir}/${name}.${variant}.spv)
            set(json ${out_dir}/${name}.${variant}.json)
            add_custom_command(
                OUTPUT ${spv} ${json}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${out_dir}
                COMMAND ${QUAT3D_SLANGC} ${src} ${module_dir}/material_pass_${variant}.slang-module
                        -I ${lib_dir} -I ${module_dir}
                        -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
                        -o ${spv} -reflection-json ${json}
                DEPENDS ${src}
                COMMENT "slangc material ${name}.slang (${variant}) -> SPIR-V + reflection"
                VERBATIM)
            list(APPEND outputs ${spv} ${json})
        endforeach()
    endforeach()
    add_custom_target(${target}_materials DEPENDS ${outputs})
    if(TARGET shaders) # engine source checkout: the driver modules are cooked there
        add_dependencies(${target}_materials shaders)
    endif()
    add_dependencies(${target} ${target}_materials)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${out_dir}
                $<TARGET_FILE_DIR:${target}>/cooked/shaders/materials
        VERBATIM)
endfunction()

function(quat_add_game_module target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "REGISTER;NAME" "SOURCES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "quat_add_game_module(${target}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES OR NOT arg_REGISTER)
        message(FATAL_ERROR "quat_add_game_module(${target}): SOURCES and REGISTER are required")
    endif()
    if(NOT arg_REGISTER MATCHES "^([A-Za-z_][A-Za-z0-9_]*::)*[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "quat_add_game_module(${target}): REGISTER must name a function, "
            "e.g. mygame::register_module (got '${arg_REGISTER}')")
    endif()
    set(name "${arg_NAME}")
    if(NOT name)
        string(REGEX REPLACE "_module$" "" name "${target}")
    endif()
    if(NOT name MATCHES "^[a-z][a-z0-9_]*$")
        message(FATAL_ERROR "quat_add_game_module(${target}): the module name '${name}' must be "
            "lowercase letters, digits and '_' (set NAME)")
    endif()
    add_library(${target} STATIC ${arg_SOURCES})
    target_link_libraries(${target} PUBLIC quat3d::engine)
    set_target_properties(${target} PROPERTIES
        QUAT_GAME_MODULE_NAME "${name}"
        QUAT_GAME_MODULE_REGISTER "${arg_REGISTER}")
endfunction()

# The generated main() of one game executable (see quat_add_game).
function(_quat_game_main target run modules project_dir out)
    set(QUAT_GAME_TARGET ${target})
    set(QUAT_GAME_RUN ${run})
    set(QUAT_GAME_PROJECT_DIR "${project_dir}")
    set(QUAT_GAME_DECLARATIONS "")
    set(QUAT_GAME_ENTRIES "")
    list(LENGTH modules QUAT_GAME_MODULE_COUNT)
    string(ASCII 59 semicolon)
    foreach(module IN LISTS modules)
        get_target_property(name ${module} QUAT_GAME_MODULE_NAME)
        get_target_property(fn ${module} QUAT_GAME_MODULE_REGISTER)
        if(NOT name OR NOT fn)
            message(FATAL_ERROR "quat_add_game(${target}): MODULES ${module} is not a "
                "quat_add_game_module() target")
        endif()
        if(fn MATCHES "^(.*)::([A-Za-z_][A-Za-z0-9_]*)$")
            string(APPEND QUAT_GAME_DECLARATIONS "namespace ${CMAKE_MATCH_1} {\n"
                "[[nodiscard]] std::expected<void, engine::core::Error> ${CMAKE_MATCH_2}("
                "engine::game::Registrar& registrar)${semicolon}\n}\n")
        else()
            string(APPEND QUAT_GAME_DECLARATIONS
                "[[nodiscard]] std::expected<void, engine::core::Error> ${fn}("
                "engine::game::Registrar& registrar)${semicolon}\n")
        endif()
        string(APPEND QUAT_GAME_ENTRIES "    {\"${name}\", &::${fn}},\n")
    endforeach()
    configure_file("${_QUAT3D_GAME_CMAKE_DIR}/quat_game_main.cpp.in" "${out}" @ONLY)
endfunction()

# The data root next to an executable (see the header), and its DLLs.
function(_quat_game_data target assets materials)
    if(TARGET shaders)
        add_dependencies(${target} shaders)
    endif()
    set(data $<TARGET_FILE_DIR:${target}>)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${QUAT3D_DATA_DIR}/cooked/shaders ${data}/cooked/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${QUAT3D_DATA_DIR}/assets/fonts ${data}/assets/fonts
        VERBATIM)
    if(assets)
        get_filename_component(abs_assets "${assets}" ABSOLUTE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${abs_assets} ${data}/assets
            VERBATIM)
    endif()
    if(materials)
        get_filename_component(abs_materials "${materials}" ABSOLUTE)
        _quat_cook_materials(${target} ${abs_materials})
    endif()
    quat_copy_runtime_dlls(${target})
endfunction()

function(quat_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "REMOTE;EDITOR" "ASSETS;MATERIALS;PROJECT_DIR"
        "SOURCES;LIBRARIES;MODULES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "quat_add_game(${target}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    set(generated OFF)
    if(arg_MODULES OR arg_PROJECT_DIR)
        set(generated ON)
    elseif(NOT arg_SOURCES)
        message(FATAL_ERROR "quat_add_game(${target}): SOURCES is required (or MODULES / PROJECT_DIR)")
    endif()
    if(NOT QUAT3D_DATA_DIR OR NOT EXISTS "${QUAT3D_DATA_DIR}")
        message(FATAL_ERROR "quat_add_game(${target}): QUAT3D_DATA_DIR is not set; find_package(quat3d) first")
    endif()

    if(NOT generated)
        # P8: the game writes main() itself.
        add_executable(${target} ${arg_SOURCES})
        target_link_libraries(${target} PRIVATE quat3d::engine ${arg_LIBRARIES})
        if(arg_REMOTE)
            target_link_libraries(${target} PRIVATE quat3d::remote)
        endif()
        if(arg_EDITOR)
            if(NOT TARGET quat3d::editor)
                message(FATAL_ERROR "quat_add_game(${target}): EDITOR, but the SDK was built without the editor")
            endif()
            target_link_libraries(${target} PRIVATE quat3d::editor)
        endif()
        _quat_game_data(${target} "${arg_ASSETS}" "${arg_MATERIALS}")
        return()
    endif()

    # G17.1: the player, shipping player and editor with the game's modules.
    set(project_dir "")
    if(arg_PROJECT_DIR)
        get_filename_component(project_dir "${arg_PROJECT_DIR}" ABSOLUTE)
        if(NOT EXISTS "${project_dir}/game.json")
            message(FATAL_ERROR "quat_add_game(${target}): PROJECT_DIR ${project_dir} has no game.json")
        endif()
    endif()
    # Distribution builds embed no build-machine path: a packaged game finds
    # game.json next to the executable (ENGINE_PACKAGE_BUILD in the engine's
    # tree, QUAT_GAME_PACKAGE_BUILD in a game's).
    set(baked_project_dir "${project_dir}")
    if(ENGINE_PACKAGE_BUILD OR QUAT_GAME_PACKAGE_BUILD)
        set(baked_project_dir "")
    endif()
    set(variants ${target} run_player quat3d::player
                 ${target}-shipping run_player quat3d::player_shipping)
    if(TARGET quat3d::player_editor)
        list(APPEND variants ${target}-editor run_editor quat3d::player_editor)
    elseif(arg_EDITOR)
        message(FATAL_ERROR "quat_add_game(${target}): EDITOR, but the SDK was built without the editor")
    endif()
    while(variants)
        list(POP_FRONT variants exe run player)
        set(main "${CMAKE_CURRENT_BINARY_DIR}/${exe}_main.cpp")
        _quat_game_main(${exe} ${run} "${arg_MODULES}" "${baked_project_dir}" "${main}")
        add_executable(${exe} ${main} ${arg_SOURCES})
        target_link_libraries(${exe} PRIVATE ${player} ${arg_MODULES} ${arg_LIBRARIES})
        _quat_game_data(${exe} "${arg_ASSETS}" "${arg_MATERIALS}")
    endwhile()
endfunction()

# Windows: copies the DLLs an executable needs next to it (the SDK's
# x64-windows dependencies are DLLs). From an installed SDK that is every DLL
# in <sdk>/deps/bin — libraries found by path (FindLua, FindOpenSSL) are not
# imported targets, so $<TARGET_RUNTIME_DLLS> alone would miss them. Use it
# for a game's test executables too. No-op elsewhere.
function(quat_copy_runtime_dlls target)
    if(NOT WIN32)
        return()
    endif()
    set(dlls)
    if(QUAT3D_SDK_ROOT)
        file(GLOB dlls "${QUAT3D_SDK_ROOT}/deps/bin/*.dll")
    endif()
    if(dlls)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${dlls} $<TARGET_FILE_DIR:${target}>
            VERBATIM)
    else()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_RUNTIME_DLLS:${target}> $<TARGET_FILE_DIR:${target}>
            COMMAND_EXPAND_LISTS)
    endif()
endfunction()
