# Link-graph half of layering_check (ROADMAP P8; docs/design/cozy-builder.md
# §3.1): no engine target links a game or sample target. A sample linked by
# target name (target_link_libraries(engine_scene PRIVATE coin_collector_lib))
# contains no games/ or examples/ path, so the text check in
# cmake/CheckLayering.cmake cannot see it; this walks the link graph instead.
#
#   set_property(GLOBAL PROPERTY QUAT3D_LAYERING_ENGINE_TARGETS <targets>)
#   set_property(GLOBAL PROPERTY QUAT3D_LAYERING_ALLOWED_LOCAL <helper targets>)
#   quat3d_check_link_layering()   # after every target exists (DEFER)
#
# Every target named in an engine target's LINK_LIBRARIES or
# INTERFACE_LINK_LIBRARIES — including inside generator expressions — must be
# an imported (third-party) target, another engine target, or a listed local
# helper. Anything else is a local non-engine target, i.e. a game, a sample
# or an app-layer library, and fails the configure.
include_guard(GLOBAL)

function(quat3d_check_link_layering)
    get_property(engine GLOBAL PROPERTY QUAT3D_LAYERING_ENGINE_TARGETS)
    get_property(helpers GLOBAL PROPERTY QUAT3D_LAYERING_ALLOWED_LOCAL)
    set(allowed ${engine} ${helpers})
    set(problems)
    foreach(target IN LISTS engine)
        if(NOT TARGET ${target})
            list(APPEND problems "${target} is listed as an engine target but does not exist")
            continue()
        endif()
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(items ${target} ${property})
            if(NOT items)
                continue()
            endif()
            foreach(item IN LISTS items)
                # Every identifier-like token, so names inside $<...> count.
                string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_.+-]*(::[A-Za-z0-9_.+-]+)*" names "${item}")
                foreach(name IN LISTS names)
                    if(NOT TARGET ${name})
                        continue()
                    endif()
                    get_target_property(aliased ${name} ALIASED_TARGET)
                    if(aliased)
                        set(name ${aliased})
                    endif()
                    get_target_property(imported ${name} IMPORTED)
                    list(FIND allowed ${name} at)
                    if(NOT imported AND at EQUAL -1)
                        list(APPEND problems
                            "${target} (${property}) links ${name}, which is not an engine library")
                    endif()
                endforeach()
            endforeach()
        endforeach()
    endforeach()
    if(problems)
        list(REMOVE_DUPLICATES problems)
        string(REPLACE ";" "\n  " text "${problems}")
        message(FATAL_ERROR "layering_check (link graph): an engine target depends on a "
            "non-engine target; the engine never depends on a game or sample:\n  ${text}")
    endif()
    list(LENGTH engine count)
    message(STATUS "layering_check (link graph): ${count} engine targets link only engine libraries and imported dependencies")
endfunction()
