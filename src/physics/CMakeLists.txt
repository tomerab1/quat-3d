# ---------------------------------------------------------------------------
# engine_physics — Jolt Physics wrapper (PhysicsWorld). Jolt is an
# implementation detail (PRIVATE); the public header is Jolt-free (PIMPL).
# ---------------------------------------------------------------------------
add_library(engine_physics STATIC
    physics_world.cpp
    physics_character.cpp
    physics_queries.cpp
    physics_events.cpp
    physics_body_control.cpp
    physics_shape_queries.cpp
    physics_joints.cpp)
# glm is PUBLIC: physics_world.hpp's API takes glm vectors/quaternions.
target_link_libraries(engine_physics PUBLIC engine_flags glm::glm)
target_link_libraries(engine_physics PRIVATE engine_core) # profiler zones + worker names (R4)
target_link_libraries(engine_physics PRIVATE Jolt::Jolt)
target_include_directories(engine_physics PUBLIC ${ENGINE_PUBLIC_INCLUDE})
