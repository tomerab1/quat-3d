// Device-free legacy self-tests wrapped as GoogleTest cases (ROADMAP S3,
// step 1). Each run_*_self_test() still lives in its engine module; step 2
// moves the bodies into tests/<module>/ as proper test cases and deletes the
// wrapper here.

#include <gtest/gtest.h>

#include "engine/animation/animator.hpp"
#include "engine/animation/clip.hpp"
#include "engine/animation/skeleton.hpp"
#include "engine/animation/state_machine.hpp"
#include "engine/asset/asset_manager.hpp"
#include "engine/nav/navmesh.hpp"
#include "engine/net/prediction.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/transport.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/remote/rpc_server.hpp"
#include "engine/scene/behavior.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/gltf_loader.hpp"
#include "engine/scene/scene.hpp"
#include "engine/terrain/generator.hpp"
#include "engine/terrain/streamer.hpp"
#include "support/expect_ok.hpp"

#ifdef ENGINE_EDITOR
#include "editor/scene_snapshot.hpp"
#endif

namespace anim = engine::animation;
namespace scn = engine::scene;

TEST(Asset, Manager) { ASSERT_OK(engine::asset::run_asset_manager_self_test()); }

TEST(Scene, Ecs) { ASSERT_OK(scn::run_ecs_self_test()); }
TEST(Scene, Systems) { ASSERT_OK(scn::run_scene_self_test()); }
#ifdef ENGINE_EDITOR
TEST(Editor, PlayModeSnapshot) { ASSERT_OK(engine::editor::run_scene_snapshot_self_test()); }
#endif

TEST(Animation, Skeleton) { ASSERT_OK(anim::run_skeleton_self_test()); }
TEST(Animation, ClipSampling) { ASSERT_OK(anim::run_clip_self_test()); }
TEST(Animation, Animator) { ASSERT_OK(anim::run_animator_self_test()); }
TEST(Animation, BlendAndAdditive) { ASSERT_OK(anim::run_blend_self_test()); }
TEST(Animation, StateMachine) { ASSERT_OK(anim::run_state_machine_self_test()); }
TEST(Animation, SkeletonLoad) { ASSERT_OK(scn::run_skeleton_load_self_test()); }
TEST(Animation, ClipLoad) { ASSERT_OK(scn::run_animation_load_self_test()); }

TEST(Physics, World) { ASSERT_OK(engine::physics::run_physics_self_test()); }
TEST(Physics, EcsBodies) { ASSERT_OK(scn::run_physics_body_self_test()); }
TEST(Physics, TerrainHeightField) { ASSERT_OK(scn::run_terrain_physics_self_test()); }
TEST(Physics, CharacterController) { ASSERT_OK(scn::run_character_self_test()); }
TEST(Physics, Triggers) { ASSERT_OK(engine::physics::run_trigger_self_test()); }

TEST(Gltf, MaterialExtract) { ASSERT_OK(scn::run_material_extract_self_test()); }
TEST(Gltf, StructuralValidation) { ASSERT_OK(scn::run_gltf_validation_self_test()); }

TEST(Terrain, Generation) { ASSERT_OK(engine::terrain::run_terrain_self_test()); }
TEST(Terrain, Streaming) { ASSERT_OK(engine::terrain::run_terrain_streaming_self_test()); }

TEST(Nav, Navmesh) { ASSERT_OK(engine::nav::run_navmesh_self_test()); }

TEST(Net, Transport) { ASSERT_OK(engine::net::run_transport_self_test()); }
TEST(Net, Replication) { ASSERT_OK(engine::net::run_replication_self_test()); }
TEST(Net, Prediction) { ASSERT_OK(engine::net::run_prediction_self_test()); }

TEST(Remote, RpcServer) { ASSERT_OK(engine::remote::run_rpc_server_self_test()); }
