#pragma once

// Colours shared by the Behaviour panel's sources (behavior_panel.cpp,
// behavior_canvas.cpp). Private editor header.

#include <imgui.h>

namespace engine::editor {

inline const ImVec4 kRunningColor(0.30F, 0.85F, 0.40F, 1.0F); // running nodes / links
inline const ImVec4 kErrorColor(0.95F, 0.35F, 0.30F, 1.0F);   // validation issues

} // namespace engine::editor
