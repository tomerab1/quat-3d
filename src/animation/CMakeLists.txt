# ---------------------------------------------------------------------------
# engine_animation — CPU-side skeletal animation (skeletons, clips, animator,
# inertialization, two-bone / foot IK, ACL clip compression). Graphics-free:
# plain data + math, consumed by engine_scene and the GPU skinning pass.
# ---------------------------------------------------------------------------
add_library(engine_animation STATIC
    skeleton.cpp
    clip.cpp
    animator.cpp
    state_machine.cpp
    inertialization.cpp
    clip_resample.cpp
    acl_compress.cpp
    acl_decompress.cpp
    ik/two_bone_ik.cpp
    ik/foot_placement.cpp)
target_link_libraries(engine_animation PUBLIC engine_flags glm::glm)
# ACL is an implementation detail of the clip codec (acl_*.cpp).
# BUILD_INTERFACE: header-only and fetched at configure time, so the SDK's
# exported engine_animation (a static library) needs nothing from it.
target_link_libraries(engine_animation PRIVATE $<BUILD_INTERFACE:acl_headers>)
target_include_directories(engine_animation PUBLIC ${ENGINE_PUBLIC_INCLUDE})
