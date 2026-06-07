# CompileShaders.cmake — offline Slang -> SPIR-V + reflection-JSON cooking.
#
# add_shader_compilation(<target>) globs assets/shaders/*.slang and, for each,
# adds a custom command running slangc to produce cooked/shaders/<name>.spv and
# cooked/shaders/<name>.json. A single custom target gathers all outputs so it
# can be a build dependency of the executable.

find_program(SLANGC_EXECUTABLE slangc REQUIRED)

function(add_shader_compilation TARGET_NAME)
    set(shader_src_dir ${CMAKE_SOURCE_DIR}/assets/shaders)
    set(shader_out_dir ${CMAKE_SOURCE_DIR}/cooked/shaders)

    file(GLOB shader_sources CONFIGURE_DEPENDS ${shader_src_dir}/*.slang)

    set(outputs)
    foreach(src ${shader_sources})
        get_filename_component(name ${src} NAME_WE)
        set(spv  ${shader_out_dir}/${name}.spv)
        set(json ${shader_out_dir}/${name}.json)
        add_custom_command(
            OUTPUT ${spv} ${json}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${shader_out_dir}
            COMMAND ${SLANGC_EXECUTABLE} ${src}
                    -target spirv -emit-spirv-directly
                    -o ${spv} -reflection-json ${json}
            DEPENDS ${src}
            COMMENT "slangc ${name}.slang -> SPIR-V + reflection"
            VERBATIM)
        list(APPEND outputs ${spv} ${json})
    endforeach()

    add_custom_target(${TARGET_NAME} ALL DEPENDS ${outputs})
endfunction()
