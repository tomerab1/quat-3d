# ---------------------------------------------------------------------------
# engine_net — GameNetworkingSockets transport (Phase 14). GNS is an
# implementation detail (PRIVATE); the public header is GNS-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_net STATIC
    character_step.cpp
    net_ids.cpp
    transport.cpp
    transport_common.cpp
    host_access.cpp
    session_config.cpp
    replication.cpp
    replication_transport.cpp
    prediction.cpp
    session.cpp)
target_link_libraries(engine_net PUBLIC engine_flags glm::glm EnTT::EnTT)
# Linux (static triplet) exports ::static; Windows (dynamic) exports ::shared.
if(TARGET GameNetworkingSockets::static)
    target_link_libraries(engine_net PRIVATE GameNetworkingSockets::static)
else()
    target_link_libraries(engine_net PRIVATE GameNetworkingSockets::shared)
endif()
target_link_libraries(engine_net PRIVATE engine_scene)
target_include_directories(engine_net PUBLIC ${ENGINE_PUBLIC_INCLUDE})
