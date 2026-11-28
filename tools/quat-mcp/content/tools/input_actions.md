List the game's input actions: the contexts, the context stack (bottom to top), and every action with its `type` (button, axis1d, axis2d), `context`, `trigger`, `bindings` (e.g. `"key:w"`, `"key:ctrl+z"`, `"mouse:left"`, `"gamepad:left_stick_xy"`), and its value this frame (`value` [x, y], `active`, `actuated`, `held_s`). Give `action` to read just one.

The default actions keep the Lua scripting names: `move_x`, `move_y`, `look_x`, `look_y`, `jump`, `fire`, `aim`, `interact`, `sprint`, `pause`. A project adds its own in `assets/input/actions.json`.
