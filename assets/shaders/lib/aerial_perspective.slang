// aerial_perspective.slang — the camera-frustum aerial perspective volume
// (ROADMAP R13.3; Hillaire 2020, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique", sec. 5.3).
//
// AerialPerspectivePass (atmos_aerial.slang) integrates the
// atmosphere's in-scattering and transmittance from the camera into a
// froxel volume of AP_SIZE² screen texels x AP_SLICES depth slices; the
// composite reads it at each opaque pixel's distance (and the cloud march
// at its clouds' distance), so distant terrain and clouds take the haze of
// the air in front of them. Slices are spaced quadratically up to AP_MAX_KM
// (finer near the camera, where geometry is); rgb is in-scattered
// luminance per unit extra-atmospheric solar irradiance (the sky-view LUT
// unit), a the mean transmittance.

public static const uint  AP_SIZE = 32;
public static const uint  AP_SLICES = 32;
public static const float AP_MAX_KM = 96.0;

// Distance (km) at normalised slice coordinate w in [0, 1].
public float ap_depth_km(float w) {
    return AP_MAX_KM * w * w;
}

public float ap_w_from_depth(float km) {
    return sqrt(saturate(km / AP_MAX_KM));
}

// The volume at screen uv and distance `km` (trilinear). In front of the
// first slice centre the value fades to "no atmosphere" (the volume holds
// none at distance 0).
public float4 sample_aerial_perspective(Texture3D ap, SamplerState samp, float2 uv, float km) {
    float w = ap_w_from_depth(km);
    float4 v = ap.SampleLevel(samp, float3(uv, w), 0.0);
    float w0 = 0.5 / float(AP_SLICES);
    if (w < w0) {
        float f = km / max(ap_depth_km(w0), 1e-6);
        v = lerp(float4(0.0, 0.0, 0.0, 1.0), v, saturate(f));
    }
    return v;
}
