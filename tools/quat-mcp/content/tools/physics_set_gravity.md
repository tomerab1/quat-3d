Set the scene's gravity, e.g. `{"gravity": [0, -1.62, 0]}` for the Moon, or `[0, 0, 0]` for zero-g. Bodies and character controllers share this one setting. Sleeping bodies wake up to feel the change. A body's `rigidbody.gravity_factor` still scales it per body. Character controllers fall along the vertical (y) component only.

Gravity is a scene setting. In edit mode it is saved with the scene (`scene_save`). Changed while playing, it applies at once and `sim_stop` restores the value from before play. The magnitude can be at most 1000 m/s². Returns `{gravity, magnitude}`.
