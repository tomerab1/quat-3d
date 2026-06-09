// sky.slang — shared analytic procedural sky (Phase 7).
//
// A horizon->zenith gradient with a ground tint below the horizon and a sun disk
// + glow toward `sun_dir`. Used as the deferred background AND as the environment
// source baked into the IBL cubemap, so both stay identical. `dir` is the view
// (or sample) ray; `sun_dir` is the direction TO the sun (normalised).

public float3 sky(float3 dir, float3 sun_dir) {
    float up = dir.y;
    float3 horizon = float3(0.75, 0.82, 0.95);
    float3 zenith = float3(0.18, 0.40, 0.78);
    float3 ground = float3(0.22, 0.20, 0.18);
    float3 col = up >= 0.0 ? lerp(horizon, zenith, pow(saturate(up), 0.45))
                           : lerp(horizon, ground, saturate(-up * 3.0));
    float sun_amt = saturate(dot(dir, sun_dir));
    float disk = pow(sun_amt, 1500.0);
    float glow = pow(sun_amt, 8.0) * 0.25;
    col += (disk * 12.0 + glow) * float3(1.0, 0.92, 0.75);
    return col;
}
