Store raw triangles as a dynamic mesh under `id`. Give `positions` (`[x, y, z]` per vertex, mesh-local metres) and `indices` (three per triangle, counter-clockwise seen from the front). Optional inputs:
- `normals`: computed from the triangles when omitted, smoothed below `crease_angle` degrees;
- `uvs`: u and v interleaved, two per vertex; box-projected in metres when omitted.

Tangents are always computed. Show the mesh with a `dynamic_mesh` component naming the id. Setting an existing id replaces its geometry: the old version keeps drawing until the new one is uploaded.

Prefer `geometry_generate` for roofs, walls, extrusions and stones. Use this tool for shapes no generator covers. Errors name the problem: an index out of range, or a count that is not a whole number of triangles.
