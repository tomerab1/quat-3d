// terrain_grid.slang — the terrain chunk's vertex-id layout (Phase 12),
// shared by the GBuffer draw (terrain.slang) and the shadow-caster draw
// (terrain_shadow.slang) so both build identical geometry.
//
// Ids [0, (n+1)²) are the (n+1)x(n+1) chunk grid; the next 4n ids walk the
// perimeter ring once more for the skirt.

public void terrain_decode_vertex(uint id, uint n, out float2 g, out bool skirt) {
    const uint grid_count = (n + 1) * (n + 1);
    if (id < grid_count) {
        g = float2(float(id % (n + 1)), float(id / (n + 1)));
        skirt = false;
        return;
    }
    skirt = true;
    uint r = id - grid_count;
    const uint side = r / n;
    const float t = float(r % n);
    const float fn = float(n);
    if (side == 0)      g = float2(t, 0.0);        // -z edge, left to right
    else if (side == 1) g = float2(fn, t);          // +x edge
    else if (side == 2) g = float2(fn - t, fn);     // +z edge, right to left
    else                g = float2(0.0, fn - t);    // -x edge
}
