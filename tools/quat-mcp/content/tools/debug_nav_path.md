Ask the navmesh for a walking path between two world points, the way NPC nav agents plan: `{points:[[x,y,z],...], partial, length}`. `partial: true` means the target is unreachable and the path stops at the closest reachable point. Use it to explain why an agent does not reach its target: query from the agent's `world_position` to its `nav_agent.target`.

Both points snap to the navmesh (within 4 m horizontally). Needs a baked navmesh (`nav_bake`); a point far off the mesh is an error.
