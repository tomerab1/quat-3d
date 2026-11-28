# ---------------------------------------------------------------------------
# engine_remote — remote-control endpoint (Phase 15): loopback JSON-RPC 2.0
# over TCP, poll-driven on the main thread. Out-of-process tools (the quat-mcp
# Model Context Protocol bridge in tools/quat-mcp) drive the engine through it.
# nlohmann_json is PUBLIC: the handler API traffics in JSON values. OpenSSL's
# libcrypto (PRIVATE) generates the session tokens (session_token.cpp).
# ---------------------------------------------------------------------------
add_library(engine_remote STATIC
    rpc_server.cpp
    session_token.cpp
    socket.cpp)
target_link_libraries(engine_remote PUBLIC engine_flags nlohmann_json::nlohmann_json)
target_link_libraries(engine_remote PRIVATE OpenSSL::Crypto engine_core) # engine_core: the log
target_include_directories(engine_remote PUBLIC ${ENGINE_PUBLIC_INCLUDE})
# The user-only token file: one implementation per platform (private_file.hpp).
if(WIN32)
    target_sources(engine_remote PRIVATE private_file_win32.cpp)
    target_link_libraries(engine_remote PRIVATE ws2_32 advapi32)
else()
    target_sources(engine_remote PRIVATE private_file_posix.cpp)
endif()
add_subdirectory(world)
