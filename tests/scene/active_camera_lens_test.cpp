// R20: the lens of a view that renders through the scene (PR #79 review). In
// builds without the editor, remote camera.get reports it and camera.set
// writes the merged pose back, so it must be the active camera's own lens.

#include <gtest/gtest.h>

#include <entt/entity/registry.hpp>

#include "engine/renderer/physical_camera.hpp"
#include "engine/scene/components.hpp"
#include "engine/scene/scene.hpp"

namespace {

namespace s = engine::scene;
using engine::renderer::PhysicalCamera;

PhysicalCamera authored_lens() {
    PhysicalCamera lens;
    lens.aperture_f = 1.4F;
    lens.focus_distance_m = 3.0F;
    lens.focal_length_mm = 85.0F;
    return lens;
}

entt::entity add_camera(entt::registry& r, bool active, const PhysicalCamera& lens) {
    const entt::entity e = r.create();
    r.emplace<s::Transform>(e);
    s::Camera cam;
    cam.is_active = active;
    cam.lens = lens;
    r.emplace<s::Camera>(e, cam);
    return e;
}

TEST(ActiveCameraLens, IsTheActiveCamerasAuthoredLens) {
    entt::registry r;
    (void)add_camera(r, true, authored_lens());
    const PhysicalCamera fly; // the fly camera's lens, never seeded from the scene
    const PhysicalCamera got = s::active_camera_lens(r, fly);
    EXPECT_FLOAT_EQ(got.aperture_f, 1.4F);
    EXPECT_FLOAT_EQ(got.focus_distance_m, 3.0F);
    EXPECT_FLOAT_EQ(got.focal_length_mm, 85.0F);
}

TEST(ActiveCameraLens, FallsBackWithoutAnActiveCamera) {
    entt::registry r;
    PhysicalCamera fly;
    fly.aperture_f = 5.6F;
    EXPECT_FLOAT_EQ(s::active_camera_lens(r, fly).aperture_f, 5.6F) << "no camera";
    (void)add_camera(r, false, authored_lens());
    EXPECT_FLOAT_EQ(s::active_camera_lens(r, fly).aperture_f, 5.6F) << "inactive camera";
}

// The round trip remote camera.set {position} performs without the editor:
// read the pose (lens from the scene), change only the position, write the
// lens back. The authored lens must survive unchanged.
TEST(ActiveCameraLens, PoseOnlyRoundTripKeepsTheAuthoredLens) {
    entt::registry r;
    const entt::entity cam = add_camera(r, true, authored_lens());
    const PhysicalCamera fly;
    const PhysicalCamera read = s::active_camera_lens(r, fly);
    r.get<s::Camera>(cam).lens = engine::renderer::sanitized(read);
    const PhysicalCamera& after = r.get<s::Camera>(cam).lens;
    EXPECT_FLOAT_EQ(after.aperture_f, 1.4F);
    EXPECT_FLOAT_EQ(after.focus_distance_m, 3.0F);
    EXPECT_FLOAT_EQ(after.focal_length_mm, 85.0F);
}

} // namespace
