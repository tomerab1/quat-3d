# quat_add_game() — build a game executable against the engine SDK (ROADMAP
# P8; installed in <sdk>/lib/cmake/quat3d and included by quat3dConfig.cmake).
#
#   quat_add_game(<target>
#       SOURCES <file>...            # the game's C++ sources (with main())
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
# Scope (docs/sdk.md): this is P8's build-and-data half. Game modules — the
# Registrar, generated registration calls and the per-game player/editor —
# arrive with ROADMAP G17.1 as quat_add_game_module() and options here.
#
# QUAT3D_DATA_DIR and QUAT3D_SLANGC come from quat3dConfig.cmake (installed
# SDK) or cmake/Quat3dInstall.cmake (engine source checkout).

include_guard(GLOBAL)

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
                COMMAND ${QUAT3D_SLANGC} -I ${lib_dir} -I ${module_dir}
                        ${src} ${module_dir}/material_pass_${variant}.slang-module
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

function(quat_add_game target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "REMOTE;EDITOR" "ASSETS;MATERIALS" "SOURCES;LIBRARIES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "quat_add_game(${target}): unknown arguments ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "quat_add_game(${target}): SOURCES is required")
    endif()
    if(NOT QUAT3D_DATA_DIR OR NOT EXISTS "${QUAT3D_DATA_DIR}")
        message(FATAL_ERROR "quat_add_game(${target}): QUAT3D_DATA_DIR is not set; find_package(quat3d) first")
    endif()

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
    if(TARGET shaders)
        add_dependencies(${target} shaders)
    endif()

    set(data $<TARGET_FILE_DIR:${target}>)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${QUAT3D_DATA_DIR}/cooked/shaders ${data}/cooked/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${QUAT3D_DATA_DIR}/assets/fonts ${data}/assets/fonts
        VERBATIM)
    if(arg_ASSETS)
        get_filename_component(assets "${arg_ASSETS}" ABSOLUTE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${assets} ${data}/assets
            VERBATIM)
    endif()
    if(arg_MATERIALS)
        get_filename_component(materials "${arg_MATERIALS}" ABSOLUTE)
        _quat_cook_materials(${target} ${materials})
    endif()
    quat_copy_runtime_dlls(${target})
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
