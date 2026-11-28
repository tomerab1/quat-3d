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

find_program(SLANGC_EXECUTABLE slangc REQUIRED)

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
            COMMAND ${SLANGC_EXECUTABLE} ${src} -I ${shader_lib_dir} -o ${mod}
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
        add_custom_command(
            OUTPUT ${spv} ${json}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out_dir}
            COMMAND ${SLANGC_EXECUTABLE} ${src} -I ${shader_lib_dir}
                    -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
                    -o ${spv} -reflection-json ${json}
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
            add_custom_command(
                OUTPUT ${spv} ${json}
                COMMAND ${CMAKE_COMMAND} -E make_directory ${material_out_dir}
                COMMAND ${SLANGC_EXECUTABLE} ${src} ${driver} -I ${shader_lib_dir}
                        -I ${shader_out_dir}
                        -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
                        -o ${spv} -reflection-json ${json}
                DEPENDS ${src} ${module_outputs}
                COMMENT "slangc material ${name}.slang (${variant}) -> SPIR-V + reflection"
                VERBATIM)
            list(APPEND outputs ${spv} ${json})
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${outputs})
endfunction()
