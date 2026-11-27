# Fails when BINARY contains Basis Universal or ASTC encoder code: the game
# links libktx's read-only library, so these symbols mean the full libktx
# (or the encoder) slipped into the shipped executable.
#   cmake -DNM=<nm> -DBINARY=<file> -P CheckNoTextureEncoder.cmake
if(NOT NM OR NOT BINARY)
    message(FATAL_ERROR "usage: cmake -DNM=<nm> -DBINARY=<file> -P CheckNoTextureEncoder.cmake")
endif()
execute_process(COMMAND "${NM}" -C "${BINARY}"
    OUTPUT_VARIABLE symbols ERROR_VARIABLE nm_error RESULT_VARIABLE nm_result)
if(NOT nm_result EQUAL 0)
    message(FATAL_ERROR "${NM} failed on ${BINARY}: ${nm_error}")
endif()
string(REGEX MATCHALL "[^\n]*(basis_compressor|astcenc_)[^\n]*" found "${symbols}")
list(LENGTH found count)
if(count GREATER 0)
    list(SUBLIST found 0 5 sample)
    string(REPLACE ";" "\n" sample "${sample}")
    message(FATAL_ERROR "${BINARY} links texture encoder code (${count} symbols), e.g.\n${sample}")
endif()
message(STATUS "${BINARY}: no texture encoder symbols")
