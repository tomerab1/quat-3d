// sh9.slang — order-3 (9 coefficient) real spherical harmonics for diffuse
// irradiance (Ramamoorthi & Hanrahan 2001; ROADMAP R21). Mirrored on the
// CPU by engine/renderer/hdri_math.hpp (sh9_basis); keep them identical.

public static const uint SH9_COUNT = 9u;
public static const float SH_PI = 3.14159265358979;

// The nine basis functions at unit direction `d`.
public void sh9_basis(float3 d, out float b[9]) {
    b[0] = 0.282095;
    b[1] = 0.488603 * d.y;
    b[2] = 0.488603 * d.z;
    b[3] = 0.488603 * d.x;
    b[4] = 1.092548 * d.x * d.y;
    b[5] = 1.092548 * d.y * d.z;
    b[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
    b[7] = 1.092548 * d.x * d.z;
    b[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// Convolution with the clamped cosine lobe divided by pi (A_l / pi:
// 1, 2/3, 1/4): projected radiance -> E / pi, the cosine-weighted average
// radiance the irradiance cube stores (lighting: diffuse = value * albedo).
public float sh9_band_weight(uint k) {
    return k == 0u ? 1.0 : (k < 4u ? 2.0 / 3.0 : 0.25);
}

// Solid angle of texel (x, y) of a `size`^2 cube face, exactly: the area of
// the face patch projected onto the sphere (Driscoll's formulation).
float sh_area_element(float x, float y) {
    return atan2(x * y, sqrt(x * x + y * y + 1.0));
}

public float cube_texel_solid_angle(uint2 texel, uint size) {
    float inv = 1.0 / float(size);
    float x0 = (2.0 * float(texel.x)) * inv - 1.0;
    float y0 = (2.0 * float(texel.y)) * inv - 1.0;
    float x1 = x0 + 2.0 * inv;
    float y1 = y0 + 2.0 * inv;
    return sh_area_element(x0, y0) - sh_area_element(x0, y1) - sh_area_element(x1, y0) +
           sh_area_element(x1, y1);
}
