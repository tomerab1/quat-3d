// pbr.slang — shared Cook-Torrance BRDF module (Phase 7, Slice 7.4).
//
// The deferred lighting pass keeps its own inline copy (pixel-exact self-tests);
// the forward transparent pass imports this so the two stay consistent.

static const float PBR_PI = 3.14159265358979;

// GGX / Trowbridge-Reitz NDF. `a` = roughness^2.
public float pbr_d_ggx(float ndoth, float a) {
    float a2 = a * a;
    float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    return a2 / (PBR_PI * d * d);
}

// Height-correlated Smith visibility (folds in 1 / (4·NdotV·NdotL)).
public float pbr_v_smith(float ndotv, float ndotl, float a) {
    float a2 = a * a;
    float gv = ndotl * sqrt(ndotv * ndotv * (1.0 - a2) + a2);
    float gl = ndotv * sqrt(ndotl * ndotl * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

public float3 pbr_f_schlick(float vdoth, float3 f0) {
    return f0 + (1.0 - f0) * pow(1.0 - vdoth, 5.0);
}

// (diffuse + specular) * NdotL for one unit light direction L (no radiance).
public float3 pbr_surface_brdf(float3 N, float3 V, float3 L, float3 albedo, float metallic,
                               float roughness) {
    float3 h = normalize(V + L);
    float ndotl = max(dot(N, L), 0.0);
    float ndotv = max(dot(N, V), 1e-4);
    float ndoth = max(dot(N, h), 0.0);
    float vdoth = max(dot(V, h), 0.0);

    float a = max(roughness * roughness, 1e-3);
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    float D = pbr_d_ggx(ndoth, a);
    float Vis = pbr_v_smith(ndotv, ndotl, a);
    float3 F = pbr_f_schlick(vdoth, f0);

    float3 spec = D * Vis * F;
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * albedo / PBR_PI;
    return (diffuse + spec) * ndotl;
}
