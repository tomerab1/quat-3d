#pragma once

// ScriptInput — the device input gameplay scripts read (G7). Since G5 it is
// the engine's input::DeviceState; scripts' virtual axes and actions
// (`input.axis`, `input.action`, ...) come from the input::Actions service
// and its action map (engine/input/actions.hpp), whose default map keeps
// G7's built-in names.

#include "engine/input/device_state.hpp"

namespace engine::script {

using ScriptInput = input::DeviceState;

using input::gamepad_axis_from_name;
using input::gamepad_button_from_name;
using input::key_from_name;
using input::mouse_button_from_name;

} // namespace engine::script
