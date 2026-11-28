# CompileShaders.cmake — offline Slang -> SPIR-V + reflection-JSON cooking.
#
# add_shader_compilation(<target>) cooks two kinds of Slang source:
#   * Library modules in assets/shaders/lib/*.slang — shared interfaces/structs
#     (e.g. material.slang) with no entry point. Each is validated and serialized
#     to cooked/shaders/<name>.slang-module, and imported by passes via
#     `import <name>;`. They are NOT compiled to SPIR-V (no entry point).
#   * Pass shaders in assets/shaders/*.slang — each has entry points and is
#     compiled to cooked/shaders/<name>.spv + <name>.json, with lib/ on the
#     import search path. Passes depend on the modules so a module edit rebuilds
#     its consumers.
#   * Material shaders in assets/shaders/materials/*.slang (ROADMAP A6) — each
#     implements IMaterial and exports `SurfaceMaterial`; it is linked against
#     every pass-variant driver module (lib/material_pass_<variant>.slang, see
#     include/engine/rhi/material_shader_names.hpp) into
#     cooked/shaders/materials/<name>.<variant>.spv + .json. The cooked
#     modules are on the search path so the link sees one copy of each.
# A single custom target gathers all outputs so it can be a build dependency of
# the executable.
#
# Shader hot reload (src/rhi/shader_compiler.cpp, ShaderCompiler::arguments)
# runs slangc with these exact flags at runtime — keep the two in sync.
# Search paths (-I) come before the inputs: slangc 2025.12+ loads a
# `.slang-module` input (the material driver) as soon as it parses it and
# resolves the module's imports against the -I paths seen so far (#136).

find_program(SLANGC_EXECUTABLE slangc REQUIRED)

# ENGINE_VALIDATE_SHADERS (top-level CMakeLists.txt, issue #136): each cooked
# .spv is checked by `spirv-val --target-env vulkan1.3` in the same build step
# that writes it, so an invalid module fails the build (and is rebuilt next
# time). The Vulkan environment rules matter: an ArrayStride on an array used
# by a Function/Workgroup variable (VUID-StandaloneSpirv-None-10684) passes
# plain spirv-val but is rejected by the validation layer. SPIRV-Tools checks
# explicit layouts per storage class from v2025.1 (v2025.2 fixed pointer
# types), so an older spirv-val -- e.g. Ubuntu 24.04's -- is refused. The
# vcpkg host tool (manifest feature `shader-validation`) is on
# CMAKE_PROGRAM_PATH and wins over PATH.
set(QUAT_SPIRV_VAL_COMMAND)
if(ENGINE_VALIDATE_SHADERS)
    find_program(SPIRV_VAL_EXECUTABLE spirv-val REQUIRED)
    execute_process(COMMAND ${SPIRV_VAL_EXECUTABLE} --version
        OUTPUT_VARIABLE spirv_val_version ERROR_QUIET)
    string(REGEX MATCH "v(20[0-9][0-9])\\.([0-9]+)" spirv_val_tag "${spirv_val_version}")
    set(spirv_val_year ${CMAKE_MATCH_1})
    set(spirv_val_minor ${CMAKE_MATCH_2})
    if(NOT spirv_val_tag OR spirv_val_year LESS 2025
            OR (spirv_val_year EQUAL 2025 AND spirv_val_minor LESS 2))
        message(FATAL_ERROR "ENGINE_VALIDATE_SHADERS needs SPIRV-Tools v2025.2 or newer "
            "(explicit-layout checks); ${SPIRV_VAL_EXECUTABLE} reports: ${spirv_val_version}")
    endif()
    message(STATUS "Cooked shaders validated by ${SPIRV_VAL_EXECUTABLE} (${spirv_val_tag})")
    set(QUAT_SPIRV_VAL_COMMAND ${SPIRV_VAL_EXECUTABLE} --target-env vulkan1.3)
endif()

# Sets <out_var> to the extra custom-command step that validates <spv>, or to
# nothing when ENGINE_VALIDATE_SHADERS is off.
function(quat_spirv_validate_command out_var spv)
    if(QUAT_SPIRV_VAL_COMMAND)
        set(${out_var} COMMAND ${QUAT_SPIRV_VAL_COMMAND} ${spv} PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

function(add_shader_compilation TARGET_NAME)
    set(shader_src_dir ${PROJECT_SOURCE_DIR}/assets/shaders)
    set(shader_lib_dir ${shader_src_dir}/lib)
    set(shader_out_dir ${PROJECT_SOURCE_DIR}/cooked/shaders)

    file(GLOB shader_sources CONFIGURE_DEPENDS ${shader_src_dir}/*.slang)
    file(GLOB shader_modules CONFIGURE_DEPENDS ${shader_lib_dir}/*.slang)
    file(GLOB material_sources CONFIGURE_DEPENDS ${shader_src_dir}/materials/*.slang)
    set(material_out_dir ${shader_out_dir}/materials)
    set(material_variants gbuffer) # driver: lib/material_pass_<variant>.slang

    set(outputs)
    set(module_outputs)

    # Library modules: syntax-validated and serialized; no SPIR-V.
    foreach(src ${shader_modules})
        get_filename_component(name ${src} NAME_WE)
        set(mod ${shader_out_dir}/${name}.slang-module)
        add_custom_command(
            OUTPUT ${mod}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out_dir}
            COMMAND ${SLANGC_EXECUTABLE} -I ${shader_lib_dir} ${src} -o ${mod}
            DEPENDS ${src}
            COMMENT "slangc module ${name}.slang -> .slang-module"
            VERBATIM)
        list(APPEND outputs ${mod})
        list(APPEND module_outputs ${mod})
    endforeach()

    # Pass shaders: SPIR-V + reflection, with lib/ on the import path.
    foreach(src ${shader_sources})
        get_filename_component(name ${src} NAME_WE)
        set(spv  ${shader_out_dir}/${name}.spv)
        set(json ${shader_out_dir}/${name}.json)
        quat_spirv_validate_command(validate ${spv})
        add_custom_command(
            OUTPUT ${spv} ${json}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out_dir}
            COMMAND ${SLANGC_EXECUTABLE} -I ${shader_lib_dir} ${src}
                    -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
                    -o ${spv} -reflection-json ${json}
            ${validate}
            DEPENDS ${src} ${module_outputs}
            COMMENT "slangc ${name}.slang -> SPIR-V + reflection"
            VERBATIM)
        list(APPEND outputs ${spv} ${json})
    endforeach()

    # Material shaders: one SPIR-V program per pass variant, linked against the
    # variant's cooked driver module.
    foreach(src ${material_sources})
        get_filename_component(name ${src} NAME_WE)
        foreach(variant ${material_variants})
            set(driver ${shader_out_dir}/material_pass_${variant}.slang-module)
            set(spv  ${material_out_dir}/${name}.${variant}.spv)
            set(json ${material_out_dir}/${name}.${variant}.json)
            quat_spirv_validate_command(validate ${spv})
            add_custom_command(
                OUTPUT ${spv} ${json}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${material_out_dir}
                COMMAND ${SLANGC_EXECUTABLE} -I ${shader_lib_dir} -I ${shader_out_dir}
                        ${src} ${driver}
                        -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
                        -o ${spv} -reflection-json ${json}
                ${validate}
                DEPENDS ${src} ${module_outputs}
                COMMENT "slangc material ${name}.slang (${variant}) -> SPIR-V + reflection"
                VERBATIM)
            list(APPEND outputs ${spv} ${json})
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${outputs})
endfunction()
