# The engine SDK's installed header set (ROADMAP P8, docs/sdk.md).
#
# Only these headers are installed, so a game built against the SDK cannot
# include anything else (docs/design/cozy-builder.md §3.1 rule 2, §3.4).
#
#   QUAT3D_SDK_STABLE_HEADERS   the stable API, covered by the versioning
#                               policy in docs/sdk.md.
#   QUAT3D_SDK_SUPPORT_HEADERS  headers the stable ones include (their types
#                               appear in stable signatures and components).
#                               Installed and versioned like the stable set;
#                               a game includes them through the stable
#                               headers, not by name.
#
# Every installed header must compile alone against the installed tree with
# only glm, EnTT and nlohmann_json (the public_headers_self_contained test)
# and must not include a header outside this list (layering_check, cmake/CheckLayering.cmake).
# Graphics-API headers (engine/rhi/*, engine/renderer/*_pass.hpp,
# vulkan/vulkan.h, vk_mem_alloc.h) are never installed: a game that needs
# rendering access gets a minimal Vulkan-free interface promoted into this set.
# engine/version.hpp is generated (cmake/version.hpp.in) and installed too.
set(QUAT3D_SDK_STABLE_HEADERS
    engine/core/error.hpp
    engine/core/json_fields.hpp
    engine/core/log.hpp
    engine/core/path_confine.hpp
    engine/core/sim_clock.hpp
    engine/core/spring.hpp
    engine/core/utf8_path.hpp
    engine/input/action_map.hpp
    engine/input/actions.hpp
    engine/input/device_state.hpp
    engine/physics/physics_world.hpp
    engine/project/game_project.hpp
    engine/remote/rpc_server.hpp
    engine/scene/camera_framing.hpp
    engine/scene/camera_rig.hpp
    engine/scene/component_json.hpp
    engine/scene/components.hpp
    engine/scene/ground_placement.hpp
    engine/scene/navigation.hpp
    engine/scene/scatter.hpp
    engine/scene/scene.hpp
    engine/scene/scene_io.hpp
    engine/spline/curve.hpp
    engine/spline/frames.hpp
    engine/spline/ground.hpp
    engine/spline/queries.hpp
    engine/spline/spline.hpp
    engine/terrain/heightmap.hpp)

set(QUAT3D_SDK_SUPPORT_HEADERS
    engine/animation/animator.hpp
    engine/animation/clip.hpp
    engine/animation/foot_placement.hpp
    engine/animation/inertialization.hpp
    engine/animation/skeleton.hpp
    engine/animation/two_bone_ik.hpp
    engine/asset/asset_manager.hpp
    engine/asset/material_params.hpp
    engine/audio/audio_types.hpp
    engine/nav/navmesh.hpp
    engine/renderer/grass.hpp
    engine/renderer/particles.hpp
    engine/renderer/physical_camera.hpp
    engine/scene/animation_system.hpp
    engine/terrain/generator.hpp)

set(QUAT3D_SDK_HEADERS ${QUAT3D_SDK_STABLE_HEADERS} ${QUAT3D_SDK_SUPPORT_HEADERS})
