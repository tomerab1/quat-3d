// equirect.slang — equirectangular (latitude-longitude) panorama mapping for
// HDRI environments (ROADMAP R21). Mirrored on the CPU by
// engine/renderer/hdri_math.hpp; keep the two identical.
//
// Row 0 of the image is the zenith (+Y), the last row the nadir; u = 0.5
// looks along -Z (the camera's default forward) and u grows toward +X:
//   u = 0.5 + atan2(d.x, -d.z) / (2 pi),  v = acos(d.y) / pi.
// An environment rotated by `angle` about +Y (counter-clockwise seen from
// above) shows the panorama's direction d0 at R(angle) d0, so a lookup of a
// world direction d reads the panorama at R(-angle) d.

public static const float EQ_PI = 3.14159265358979;

public float2 equirect_uv(float3 d) {
    float u = 0.5 + atan2(d.x, -d.z) / (2.0 * EQ_PI);
    float v = acos(clamp(d.y, -1.0, 1.0)) / EQ_PI;
    return float2(u, v);
}

// R(-angle) d for `cs` = (cos angle, sin angle): world -> panorama space.
public float3 world_to_panorama(float3 d, float2 cs) {
    return float3(cs.x * d.x - cs.y * d.z, d.y, cs.y * d.x + cs.x * d.z);
}

// Rec. 709 luminance of linear RGB.
public float eq_luminance(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}
