# Fails when the shipping player (quat-player-shipping, ROADMAP P4) contains
# remote-control code: the JSON-RPC server, the world commands or the remote
# screenshot path. Shipping games must not be drivable over the network.
#   cmake -DNM=<nm> -DBINARY=<file> -P CheckNoRemoteControl.cmake   (symbol table)
#   cmake -DMAP=<linker .map> -P CheckNoRemoteControl.cmake         (MSVC)
if(MAP)
    if(NOT EXISTS "${MAP}")
        message(FATAL_ERROR "linker map ${MAP} not found (quat-player-shipping links with /MAP)")
    endif()
    file(READ "${MAP}" symbols)
    set(source "${MAP}")
elseif(NM AND BINARY)
    execute_process(COMMAND "${NM}" -C "${BINARY}"
        OUTPUT_VARIABLE symbols ERROR_VARIABLE nm_error RESULT_VARIABLE nm_result)
    if(NOT nm_result EQUAL 0)
        message(FATAL_ERROR "${NM} failed on ${BINARY}: ${nm_error}")
    endif()
    set(source "${BINARY}")
else()
    message(FATAL_ERROR "usage: -DNM=<nm> -DBINARY=<file> or -DMAP=<map> -P CheckNoRemoteControl.cmake")
endif()
string(LENGTH "${symbols}" size)
if(size EQUAL 0)
    message(FATAL_ERROR "${source}: no symbols to inspect")
endif()
# Demangled (nm -C) and MSVC-decorated spellings of the remote namespaces and
# entry points.
string(REGEX MATCHALL
    "[^\n]*(engine::remote::|@remote@engine@|RpcServer|register_world_commands|ScreenshotCapture)[^\n]*"
    found "${symbols}")
list(LENGTH found count)
if(count GREATER 0)
    list(SUBLIST found 0 5 sample)
    string(REPLACE ";" "\n" sample "${sample}")
    message(FATAL_ERROR "${source} links remote-control code (${count} symbols), e.g.\n${sample}")
endif()
message(STATUS "${source}: no remote-control symbols")
