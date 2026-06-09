// sky.slang — procedural physical-ish sky: atmosphere gradient, sun disk + Mie
// glow, and FBM clouds (Phase 7). Used as the deferred background AND baked into
// the IBL environment cube, so the reflections show the same sky. The clouds are
// static (no time) so the live background and the one-time IBL bake agree.

// --- value noise -----------------------------------------------------------
static float sky_hash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

static float sky_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = sky_hash(i + float2(0.0, 0.0));
    float b = sky_hash(i + float2(1.0, 0.0));
    float c = sky_hash(i + float2(0.0, 1.0));
    float d = sky_hash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

static float sky_fbm(float2 p) {
    float v = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 5; ++i) {
        v += amp * sky_noise(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return v;
}

// `dir` is the view/sample ray, `sun_dir` the direction TO the sun (normalised).
public float3 sky(float3 dir, float3 sun_dir) {
    float up = dir.y;

    // Atmosphere: saturated blue zenith fading to a bright hazy horizon, with a
    // dim ground tint below.
    float3 zenith  = float3(0.08, 0.28, 0.66);
    float3 horizon = float3(0.62, 0.76, 0.92);
    float3 ground  = float3(0.28, 0.26, 0.24);
    float3 col = up >= 0.0 ? lerp(horizon, zenith, pow(saturate(up), 0.42))
                           : lerp(horizon, ground, saturate(-up * 4.0));

    // Sun: a crisp disk plus a Mie-like forward glow that brightens the haze.
    float sd = saturate(dot(dir, sun_dir));
    float disk = smoothstep(0.9990, 0.9994, sd);
    float glow = pow(sd, 350.0) * 0.5 + pow(sd, 8.0) * 0.12;
    float3 sun_col = float3(1.0, 0.94, 0.82);
    col += (disk * 12.0 + glow) * sun_col;

    // Clouds: an FBM layer projected onto a plane above the horizon. Faded in over
    // the horizon band to hide the planar projection's singularity at dir.y -> 0.
    if (up > 0.015) {
        float2 cuv = dir.xz / up * 0.45 + float2(2.3, 1.1);
        float density = sky_fbm(cuv);
        float coverage = smoothstep(0.50, 0.95, density);
        float fade = saturate((up - 0.015) / 0.22);
        float amount = coverage * fade;
        float shade = saturate(density * 1.3);
        float3 cloud = lerp(float3(0.34, 0.37, 0.43), float3(1.0, 1.0, 1.0), shade);
        cloud += sun_col * glow * 0.6; // silver lining toward the sun
        col = lerp(col, cloud, amount);
    }
    return col;
}
