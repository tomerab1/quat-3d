// weather_surface.slang — weather layers over the deferred surface (R13.1).
//
// The lighting pass decodes each GBuffer pixel into a ShadingSurface and runs
// it through the weather's surface layers before shading. Layers implement
// ISurfaceLayer (Slang interfaces, no #ifdef variants); the pass applies
// WetLayer then SnowLayer, so snow lies on top of wet ground.
//
//   WetLayer   Sébastien Lagarde, "Water drop 3b - Physically based wet
//              surfaces" (2013): a porous surface soaks water and darkens
//              (diffuse x lerp(1, 0.2, porosity)) while its gloss rises toward
//              the water film's; porosity follows from the dry material
//              (rough dielectrics are porous, glossy ones and metals are
//              not). Where water accumulates (Lagarde's "flood" level) on flat
//              ground, a procedural puddle height field below the water level
//              turns the surface into still water: flat normal, roughness of
//              calm water, the dielectric reflectance of water (f0 = 0.02),
//              no metal showing through.
//   SnowLayer  Snow settles on upward-facing surfaces: coverage grows with
//              the accumulated amount and with height above a snowline that
//              descends as snow accumulates (the terrain's snowline, so the
//              weather's snow continues the terrain's own snow cap with the
//              same albedo and roughness). Facing up: the threshold on the
//              normal's y falls as coverage grows; value noise breaks the edge.

// The surface the lighting pass shades.
public struct ShadingSurface {
    public float3 albedo;
    public float3 normal;      // world, unit
    public float  metallic;
    public float  roughness;   // perceptual
    public float  dielectric_f0; // specular reflectance of the non-metal part (0.04)
}

public interface ISurfaceLayer {
    void apply(inout ShadingSurface s, float3 world_pos);
}

public void apply_surface_layer<L : ISurfaceLayer>(L layer, inout ShadingSurface s,
                                                   float3 world_pos) {
    layer.apply(s, world_pos);
}

// Matches terrain.slang's snow so the weather's snow continues the terrain's.
public static const float3 SNOW_ALBEDO = float3(0.83, 0.86, 0.90);
public static const float  SNOW_ROUGHNESS = 0.55;
// How far the snowline descends at full accumulation (m).
public static const float SNOWLINE_DROP_M = 400.0;

static const float WATER_F0 = 0.02;        // IOR 1.33
static const float WATER_ROUGHNESS = 0.03; // calm water (ripples arrive with rain, R13.2)

// Sin-free hash12 + value noise (as terrain.slang / atmosphere.slang).
float weather_hash(float2 p) {
    float3 q = frac(float3(p.x, p.y, p.x) * 0.1031);
    q += dot(q, float3(q.y, q.z, q.x) + 33.33);
    return frac((q.x + q.y) * q.z);
}

float weather_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(weather_hash(i), weather_hash(i + float2(1, 0)), u.x),
                lerp(weather_hash(i + float2(0, 1)), weather_hash(i + float2(1, 1)), u.x), u.y);
}

// Puddle height field in [0, 1]: lower values fill first. Metre-scale
// puddles grouped in ~10 m patches; the contrast is stretched so a water
// level of 0.5 floods about half the ground and the patches stay distinct.
float puddle_height(float2 xz) {
    float macro = weather_noise(xz * 0.09 + float2(31.7, 11.3));
    float local = weather_noise(xz * 0.45) * 0.6 + weather_noise(xz * 1.3 + 7.1) * 0.4;
    return saturate(0.5 + (local * 0.65 + macro * 0.35 - 0.5) * 2.0);
}

public struct WetLayer : ISurfaceLayer {
    public float wetness; // 0..1, soaking of porous surfaces
    public float water;   // 0..1, puddle water level on flat ground

    public void apply(inout ShadingSurface s, float3 world_pos) {
        if (wetness <= 0.0) return;
        // Lagarde: rough dielectrics are porous; glossy surfaces and metals
        // are not.
        float porosity = saturate((s.roughness - 0.5) / 0.4) * (1.0 - s.metallic);
        float factor = lerp(1.0, 0.2, porosity);
        s.albedo *= lerp(1.0, factor, wetness);
        float gloss = 1.0 - s.roughness;
        gloss = lerp(1.0, gloss, lerp(1.0, factor, 0.5 * wetness));
        s.roughness = 1.0 - gloss;

        // Accumulated water: flat ground below the water level.
        if (water <= 0.0) return;
        float flat = smoothstep(0.94, 0.985, s.normal.y);
        if (flat <= 0.0) return;
        float depth = water - puddle_height(world_pos.xz);
        float a = flat * smoothstep(0.0, 0.06, depth); // shore -> open water
        s.roughness = lerp(s.roughness, WATER_ROUGHNESS, a);
        s.metallic = lerp(s.metallic, 0.0, a);
        s.dielectric_f0 = lerp(s.dielectric_f0, WATER_F0, a);
        s.normal = normalize(lerp(s.normal, float3(0.0, 1.0, 0.0), a));
    }
}

public struct SnowLayer : ISurfaceLayer {
    public float amount;   // 0..1 accumulated snow
    public float snowline; // the terrain's snowline (m); very high without terrain

    public void apply(inout ShadingSurface s, float3 world_pos) {
        if (amount <= 0.0) return;
        float n = weather_noise(world_pos.xz * 0.9) * 0.7 + weather_noise(world_pos.xz * 3.7) * 0.3;
        // Deeper above the (descending) snowline.
        float line = snowline - amount * SNOWLINE_DROP_M;
        float high = smoothstep(line - 40.0, line + 40.0, world_pos.y + (n - 0.5) * 30.0);
        float cover = saturate(amount + high * saturate(amount * 4.0) * 0.6);
        // Facing up: steeper faces hold snow as the cover deepens.
        float threshold = 1.0 - 0.8 * cover;
        float mask = saturate((s.normal.y - threshold) / 0.12 + (n - 0.5) * 0.8) *
                     saturate(cover * 3.0);
        if (mask <= 0.0) return;
        s.albedo = lerp(s.albedo, SNOW_ALBEDO, mask);
        s.roughness = lerp(s.roughness, SNOW_ROUGHNESS, mask);
        s.metallic = lerp(s.metallic, 0.0, mask);
        s.dielectric_f0 = lerp(s.dielectric_f0, 0.04, mask);
        // Snow fills the surface's small relief.
        s.normal = normalize(lerp(s.normal, float3(0.0, 1.0, 0.0), 0.5 * mask));
    }
}
