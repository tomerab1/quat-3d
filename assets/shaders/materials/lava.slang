// lava.slang — example material shader (ROADMAP A6): slowly flowing molten
// rock. Dark crust plates drift over glowing cracks; the glow colour is
// shader_params[0].rgb (default orange when black) times shader_params[0].w
// (intensity, default 6), and shader_params[1].x scales the flow speed.

import material;

float hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

// Value noise with smooth interpolation.
float value_noise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + float2(1.0, 0.0));
    float c = hash21(i + float2(0.0, 1.0));
    float d = hash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int octave = 0; octave < 4; ++octave) {
        sum += amp * value_noise(p);
        p = p * 2.03 + float2(17.1, 9.2);
        amp *= 0.5;
    }
    return sum;
}

struct Lava : IMaterial {
    static SurfaceSample evaluate(MaterialInput input) {
        SurfaceSample s = input.standard;
        float4 glow_param = input.params.shader_params[0];
        float speed = input.params.shader_params[1].x > 0.0 ? input.params.shader_params[1].x
                                                             : 1.0;
        float3 glow = any(glow_param.rgb > 0.0) ? glow_param.rgb : float3(1.0, 0.35, 0.05);
        float intensity = glow_param.w > 0.0 ? glow_param.w : 6.0;

        float t = input.time * 0.15 * speed;
        float2 p = input.uv * 6.0 + float2(t, t * 0.6);
        float warp = fbm(p + fbm(p * 0.5 - t));
        float crack = smoothstep(0.55, 0.40, abs(warp - 0.5) * 2.0 + 0.35);

        s.albedo = lerp(float3(0.05, 0.04, 0.035), float3(0.02, 0.01, 0.01), crack);
        s.metallic = 0.0;
        s.roughness = lerp(0.9, 0.6, crack);
        s.emissive = glow * intensity * crack * (0.75 + 0.25 * sin(input.time * 3.0 + warp * 9.0));
        return s;
    }
}

export struct SurfaceMaterial : IMaterial = Lava;
