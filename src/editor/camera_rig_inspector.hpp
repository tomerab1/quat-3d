#pragma once

// Inspector editors for the camera rig components (G15.1). Private editor
// header; inspector.cpp's ComponentInspector specialisations delegate here.
// Both draw property-grid rows (call inside ui::begin_property_grid).

namespace engine::scene {
struct CameraRig;
struct CameraRigInput;
} // namespace engine::scene

namespace engine::editor {

// Targets, zoom / pitch limits, smoothing, ground and bounds, plus the
// damped pose the rig is at now (read-only).
void draw_camera_rig(scene::CameraRig& rig);

// The G5 action names that drive the rig and their speeds.
void draw_camera_rig_input(scene::CameraRigInput& input);

} // namespace engine::editor
